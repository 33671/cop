/*
 * stream_curl.h
 *
 * Internal module for curl child-process management: fork, pipe setup,
 * curl execution, retry logic, and cancellation.
 * NOT part of the public API - do not install.
 */

#ifndef STREAM_CURL_H
#define STREAM_CURL_H

#include "openai_stream_client.h"
#include "cjson/cJSON.h"
#include "libmill/libmill.h"

/* Child process exit codes used to communicate error type to parent. */
#define CHILD_EXIT_OK          0   /* HTTP 2xx - success              */
#define CHILD_EXIT_NET_ERR     1   /* curl/network error - retryable  */
#define CHILD_EXIT_HTTP_4XX    2   /* HTTP 4xx - not retryable        */
#define CHILD_EXIT_HTTP_5XX    3   /* HTTP 5xx - retryable            */
#define CHILD_EXIT_HTTP_429    4   /* HTTP 429 - retryable (rate limit) */

/*
 * Start a streaming chat request by forking a curl child process.
 *
 * messages: cJSON Array of message objects.
 *   The function copies the array internally; caller retains ownership.
 *
 * On HTTP 5xx / 429 / network errors, the request is retried up to
 * c->max_retries times (with c->retry_delay_ms between attempts).
 * All response data is buffered into c->main_buffer.
 *
 * Must be called from a libmill coroutine.
 * Returns 0 on success, -1 on error.
 */
coroutine int stream_curl_start_chat(stream_client_t *c, cJSON *messages);

/*
 * Cancel the running curl child process (SIGKILL).
 */
void stream_curl_cancel(stream_client_t *c);

/*
 * Wait for the curl child process to finish and reap it.
 * Closes the pipe after completion.
 */
void stream_curl_wait_done(stream_client_t *c);

#endif /* STREAM_CURL_H */
