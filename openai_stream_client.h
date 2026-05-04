/*
 * openai_stream_client.h
 * 
 * Async streaming client for OpenAI-compatible APIs.
 * Uses libcurl in a forked child process with libmill coroutines for async iteration.
 * 
 * Architecture:
 * - Child process runs curl and writes HTTP stream data to a pipe
 * - Parent process (libmill coroutines) reads from pipe via fdwait()
 * - Cancellation: kill(child_pid, SIGKILL) stops the request immediately
 */

#ifndef OPENAI_STREAM_CLIENT_H
#define OPENAI_STREAM_CLIENT_H

#include "openai_sse_parser.h"
#include "libmill/libmill.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Client State
 * ============================================================================ */
typedef enum {
    CLIENT_STATE_IDLE = 0,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_STREAMING,
    CLIENT_STATE_DONE,
    CLIENT_STATE_ERROR
} client_state_t;

/* ============================================================================
 * Stream Client Configuration
 * ============================================================================ */
typedef struct stream_client {
    /* Configuration */
    char *api_key;
    char *model;
    char *api_endpoint;
    char *system_message;
    double temperature;
    
    /* CURL child process */
    pid_t curl_pid;
    
    /* State */
    volatile int running;
    client_state_t state;
    int error_code;
    char *error_message;
    int done;
    
    /* Pipe for data transfer (child process -> parent process) */
    int data_pipe[2];                 /* [0]=read end, [1]=write end */
    
    /* Parent process buffer */
    stream_buffer_t main_buffer;
    
    /* Stats */
    size_t total_bytes;
    int64_t start_time;
    int64_t first_token_time;
    int64_t end_time;
    
    /* Tool schemas (cJSON array of tool definition objects, owned by client) */
    cJSON *tool_schemas;
    
    /* Retry configuration */
    int max_retries;                 /* max retry attempts (default 3) */
    int retry_delay_ms;              /* delay between retries in ms (default 1000) */
    
    /* Logging */
    FILE *log_fp;
    char *log_filename;
} stream_client_t;

/* ============================================================================
 * Client Lifecycle
 * ============================================================================ */

/*
 * Create a new stream client.
 * api_key: API key for authentication (required)
 * model: Model name (required)
 * api_endpoint: Full URL endpoint (NULL for default Moonshot)
 * log_file: Path to log file (NULL for no logging)
 */
stream_client_t *stream_client_new(const char *api_key, 
                                    const char *model,
                                    const char *api_endpoint, 
                                    const char *log_file);

/*
 * Free a stream client and all associated resources.
 * This will cancel any ongoing request and wait for cleanup.
 */
void stream_client_free(stream_client_t *c);

/*
 * Configure optional parameters.
 */
void stream_client_set_system_message(stream_client_t *c, const char *message);
void stream_client_set_temperature(stream_client_t *c, double temp);

/*
 * Set tool schemas for the request body.
 * schemas: cJSON Array of tool definition objects, e.g.
 *   [{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}]
 * The array is deep-copied internally; caller retains ownership.
 * Pass NULL to clear.
 */
void stream_client_set_tool_schemas(stream_client_t *c, const cJSON *schemas);

/*
 * Configure retry behaviour.
 * max_retries: 0 = no retries, -1 = use default (3)
 * delay_ms: milliseconds between retries (default 1000)
 */
void stream_client_set_max_retries(stream_client_t *c, int max_retries);
void stream_client_set_retry_delay(stream_client_t *c, int delay_ms);

/* ============================================================================
 * Streaming API
 * ============================================================================ */

/*
 * Start a streaming chat request.
 *
 * messages: cJSON Array of message objects, e.g.
 *   [{"role":"system","content":"..."}, {"role":"user","content":"..."}]
 *   May be NULL to send only the configured system message.
 *   The function copies the array internally; caller retains ownership.
 *
 * The full request body is built from client config (model, temperature,
 * system_message) plus the provided messages, with stream=true.
 *
 * This forks a child process running libcurl via mfork().
 * On HTTP 5xx / 429 / network errors, the request is retried up to
 * c->max_retries times (with c->retry_delay_ms between attempts).
 * All response data is buffered into main_buffer before returning.
 *
 * Must be called from a libmill coroutine.
 * Returns 0 on success, -1 on error (including after all retries exhausted).
 */
coroutine int stream_client_start_chat(stream_client_t *c, cJSON *messages);

/*
 * Check if there are more chunks available.
 * Returns 1 if more chunks coming, 0 if stream is complete.
 */
int stream_client_has_more(stream_client_t *c);

/*
 * Cancel an ongoing streaming request.
 * Kills the child process running curl via SIGKILL.
 */
void stream_client_cancel(stream_client_t *c);

/*
 * Wait for the streaming request to complete.
 * Blocks until child process finishes (waitpid).
 */
void stream_client_wait_done(stream_client_t *c);

/* ============================================================================
 * Async Chunk Iteration (libmill)
 * ============================================================================ */

/*
 * Wait for and extract the next chunk from the stream.
 * This is an async function that uses libmill's fdwait() to wait for data.
 * 
 * Usage in a coroutine:
 *   StreamChunk chunk;
 *   while (next_chunk(c, &chunk)) {
 *       // process chunk
 *       stream_chunk_cleanup(&chunk);
 *   }
 * 
 * Returns 1 if chunk received, 0 if stream ended (chunk is invalid).
 * 
 * Note: The caller must call stream_chunk_cleanup() on received chunks.
 */
coroutine int next_chunk(stream_client_t *c, StreamChunk *chunk);

/*
 * Check if a chunk is available without waiting.
 * Returns 1 if chunk extracted, 0 if no chunk available.
 */
coroutine int next_chunk_nowait(stream_client_t *c, StreamChunk *chunk);

/* ============================================================================
 * Blocking API (for simple use cases)
 * ============================================================================ */

/*
 * Send a prompt and block until complete response is received.
 * Calls callback for each chunk.
 * Returns 0 on success, -1 on error.
 */
typedef void (*stream_chunk_callback_t)(const StreamChunk *chunk, void *user_data);

int stream_client_chat_blocking(stream_client_t *c, 
                                 const char *prompt,
                                 stream_chunk_callback_t callback,
                                 void *user_data);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Get current state */
client_state_t stream_client_get_state(stream_client_t *c);
const char *stream_client_get_state_string(stream_client_t *c);

/* Get stats */
int64_t stream_client_get_time_to_first_token(stream_client_t *c);
int64_t stream_client_get_total_duration(stream_client_t *c);
size_t stream_client_get_total_bytes(stream_client_t *c);

/* Get error info */
int stream_client_get_error_code(stream_client_t *c);
const char *stream_client_get_error_message(stream_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* OPENAI_STREAM_CLIENT_H */
