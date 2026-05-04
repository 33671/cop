/*
 * openai_stream_client.c
 * 
 * Async streaming client for OpenAI-compatible APIs.
 * Uses libcurl in a forked child process with libmill coroutines for async iteration.
 * 
 * Process Communication:
 * - Child process runs curl and writes HTTP stream data to a pipe
 * - Parent process uses fdwait() on pipe to wait for data
 * - Cancellation kills the child process via SIGKILL, pipe EOF signals completion
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <curl/curl.h>

#include "openai_stream_client.h"

#define LOG_TIMESTAMP_FMT "%Y-%m-%d %H:%M:%S"

/* ============================================================================
 * Logging Functions
 * ============================================================================ */
static int64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void get_timestamp_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    strftime(buf, size, LOG_TIMESTAMP_FMT, tm_info);
    size_t len = strlen(buf);
    snprintf(buf + len, size - len, ".%03d", (int)(tv.tv_usec / 1000));
}

static void log_write(stream_client_t *c, const char *format, ...) {
    if (!c || !c->log_fp) return;
    
    char timestamp[64];
    get_timestamp_str(timestamp, sizeof(timestamp));
    
    fprintf(c->log_fp, "[%s] ", timestamp);
    
    va_list args;
    va_start(args, format);
    vfprintf(c->log_fp, format, args);
    va_end(args);
    
    fprintf(c->log_fp, "\n");
    fflush(c->log_fp);
}

static void log_raw_data(stream_client_t *c, const char *data, size_t len) {
    if (!c || !c->log_fp) return;
    
    char timestamp[64];
    get_timestamp_str(timestamp, sizeof(timestamp));
    
    fprintf(c->log_fp, "[%s] [RAW] ", timestamp);
    
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            fprintf(c->log_fp, "\\n");
        } else if (ch == '\r') {
            fprintf(c->log_fp, "\\r");
        } else if (ch == '\t') {
            fprintf(c->log_fp, "\\t");
        } else if (ch < 32 || ch > 126) {
            fprintf(c->log_fp, "\\x%02x", (unsigned char)ch);
        } else {
            fputc(ch, c->log_fp);
        }
    }
    
    fprintf(c->log_fp, "\n");
    fflush(c->log_fp);
}

/* ============================================================================
 * CURL Write Callback (runs in child process)
 * ============================================================================ */

/*
 * Write callback: curl calls this with received HTTP body data.
 * We write directly to the pipe; the parent process reads it.
 * Returns 0 to signal error to curl (e.g. if pipe is broken).
 */
static size_t client_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    stream_client_t *c = (stream_client_t *)userdata;
    
    c->total_bytes += total;
    log_raw_data(c, ptr, total);
    
    /* Write raw data to the pipe. If the pipe is full, this blocks until
       the parent process reads data. That provides backpressure. */
    ssize_t written = write(c->data_pipe[1], ptr, total);
    if (written != (ssize_t)total) {
        /* EPIPE means parent closed the pipe (cancellation) */
        if (errno == EPIPE) {
            log_write(c, "Pipe closed by parent (cancelled)");
        } else {
            log_write(c, "Failed to write all data to pipe (errno=%d)", errno);
        }
        return 0;   /* signal error to CURL */
    }
    
    return total;
}

/* ============================================================================
 * Client Lifecycle
 * ============================================================================ */
stream_client_t *stream_client_new(const char *api_key, 
                                    const char *model,
                                    const char *api_endpoint, 
                                    const char *log_file) {
    if (!api_key || !model) {
        return NULL;
    }
    
    stream_client_t *c = calloc(1, sizeof(stream_client_t));
    if (!c) return NULL;
    
    c->api_key = strdup(api_key);
    c->model = strdup(model);
    c->api_endpoint = strdup(api_endpoint ? api_endpoint : 
                              "https://api.moonshot.cn/v1/chat/completions");
    c->system_message = strdup("You are a helpful assistant.");
    c->temperature = 1.0;
    
    c->state = CLIENT_STATE_IDLE;
    c->running = 0;
    c->done = 0;
    c->error_code = 0;
    c->curl_pid = -1;
    c->data_pipe[0] = -1;
    c->data_pipe[1] = -1;
    c->max_retries = 3;
    c->retry_delay_ms = 1000;
    
    /* Initialize parent process buffer */
    if (stream_buffer_init(&c->main_buffer) != 0) {
        free(c);
        return NULL;
    }
    
    /* Create data pipe */
    if (pipe(c->data_pipe) != 0) {
        stream_buffer_free(&c->main_buffer);
        free(c);
        return NULL;
    }
    
    /* Make read end non-blocking for fdwait; write end can stay blocking */
    int flags = fcntl(c->data_pipe[0], F_GETFL, 0);
    fcntl(c->data_pipe[0], F_SETFL, flags | O_NONBLOCK);
    
    if (log_file) {
        c->log_filename = strdup(log_file);
        c->log_fp = fopen(log_file, "a");
        if (c->log_fp) {
            char timestamp[64];
            get_timestamp_str(timestamp, sizeof(timestamp));
            fprintf(c->log_fp, "\n============================================================\n");
            fprintf(c->log_fp, "[%s] CLIENT CREATED\n", timestamp);
            fprintf(c->log_fp, "Model: %s\n", c->model);
            fprintf(c->log_fp, "============================================================\n");
            fflush(c->log_fp);
        }
    }
    
    return c;
}

void stream_client_free(stream_client_t *c) {
    if (!c) return;
    
    /* Cancel any ongoing request (kills child process) */
    stream_client_cancel(c);
    
    /* Wait for child process to finish (reap zombie) */
    if (c->curl_pid > 0) {
        int status;
        log_write(c, "Waiting for child process (pid=%d) to exit", c->curl_pid);
        waitpid(c->curl_pid, &status, 0);
        c->curl_pid = -1;
    }
    
    /* Close data pipe */
    if (c->data_pipe[0] >= 0) {
        log_write(c, "%s:%d,closing read pipe",__FILE__,__LINE__);
        fdclean(c->data_pipe[0]);
        close(c->data_pipe[0]);
    }
    if (c->data_pipe[1] >= 0) {
        log_write(c, "%s:%d,closing write pipe",__FILE__,__LINE__);
        close(c->data_pipe[1]);
    }
    
    stream_buffer_free(&c->main_buffer);
    
    if (c->log_fp) {
        char timestamp[64];
        get_timestamp_str(timestamp, sizeof(timestamp));
        fprintf(c->log_fp, "\n============================================================\n");
        fprintf(c->log_fp, "[%s] CLIENT DESTROYED\n", timestamp);
        fprintf(c->log_fp, "Total bytes: %zu\n", c->total_bytes);
        fprintf(c->log_fp, "============================================================\n\n");
        fclose(c->log_fp);
    }
    
    free(c->api_key);
    free(c->model);
    free(c->api_endpoint);
    free(c->system_message);
    free(c->log_filename);
    cJSON_Delete(c->tool_schemas);
    free(c->error_message);
    free(c);
}

void stream_client_set_system_message(stream_client_t *c, const char *message) {
    if (!c || !message) return;
    free(c->system_message);
    c->system_message = strdup(message);
}

void stream_client_set_temperature(stream_client_t *c, double temp) {
    if (!c) return;
    c->temperature = temp;
}

void stream_client_set_tool_schemas(stream_client_t *c, const cJSON *schemas) {
    if (!c) return;
    cJSON_Delete(c->tool_schemas);
    c->tool_schemas = schemas ? cJSON_Duplicate(schemas, 1) : NULL;
}

void stream_client_set_max_retries(stream_client_t *c, int max_retries) {
    if (!c) return;
    c->max_retries = (max_retries >= 0) ? max_retries : 3;
}

void stream_client_set_retry_delay(stream_client_t *c, int delay_ms) {
    if (!c) return;
    c->retry_delay_ms = (delay_ms >= 0) ? delay_ms : 1000;
}

/* ============================================================================
 * Request Body Builder
 * ============================================================================ */

/*
 * Build a full OpenAI-compatible request body JSON string from client config
 * and a cJSON messages array.
 *
 * messages: cJSON Array of message objects (e.g. [{"role":"user","content":"hi"}])
 *           May be NULL if no user messages (only system message will be sent).
 * Returns: malloc'd JSON string, or NULL on error. Caller must free().
 */
static char *build_request_body(stream_client_t *c, cJSON *messages) {
    if (!c) return NULL;
    
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    
    /* Model */
    cJSON_AddStringToObject(root, "model", c->model ? c->model : "");
    
    /* Messages array */
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (!msgs) { cJSON_Delete(root); return NULL; }
    
    /* Prepend system message if configured and not already present
       as the first message in the caller's array */
    if (c->system_message && c->system_message[0]) {
        int has_system = 0;
        if (messages && cJSON_GetArraySize(messages) > 0) {
            cJSON *first = cJSON_GetArrayItem(messages, 0);
            cJSON *role = cJSON_GetObjectItem(first, "role");
            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "system") == 0) {
                has_system = 1;
            }
        }
        if (!has_system) {
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", c->system_message);
            cJSON_AddItemToArray(msgs, sys);
        }
    }
    
    /* Copy caller's messages into the array */
    if (messages) {
        int size = cJSON_GetArraySize(messages);
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(messages, i);
            cJSON *copy = cJSON_Duplicate(item, 1);
            if (copy) cJSON_AddItemToArray(msgs, copy);
        }
    }
    
    /* Tool schemas */
    if (c->tool_schemas && cJSON_IsArray(c->tool_schemas) &&
        cJSON_GetArraySize(c->tool_schemas) > 0) {
        cJSON_AddItemToObject(root, "tools", cJSON_Duplicate(c->tool_schemas, 1));
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }
    
    /* Streaming configuration */
    cJSON_AddBoolToObject(root, "stream", 1);
    cJSON_AddNumberToObject(root, "temperature", c->temperature);
    cJSON_AddNumberToObject(root, "max_tokens", 4096);
    
    /* Serialize to string */
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ============================================================================
 * Streaming API
 * ============================================================================ */

/* Child process exit codes used to communicate error type to parent. */
#define CHILD_EXIT_OK          0   /* HTTP 2xx — success              */
#define CHILD_EXIT_NET_ERR     1   /* curl/network error — retryable  */
#define CHILD_EXIT_HTTP_4XX    2   /* HTTP 4xx — not retryable        */
#define CHILD_EXIT_HTTP_5XX    3   /* HTTP 5xx — retryable            */
#define CHILD_EXIT_HTTP_429    4   /* HTTP 429 — retryable (rate limit) */

/*
 * Check if buffered data looks like an SSE stream (starts with "data:"
 * after optional leading whitespace).  Returns 1 if yes, 0 otherwise.
 */
static int buffer_looks_like_sse(stream_buffer_t *buf) {
    const char *p = buf->data;
    size_t rem = buf->len;
    while (rem > 0 && (*p == '\n' || *p == '\r' || *p == ' ')) {
        p++; rem--;
    }
    return (rem >= 5 && strncmp(p, "data:", 5) == 0);
}

coroutine int stream_client_start_chat(stream_client_t *c, cJSON *messages) {
    if (!c) return -1;

    /* ── Reset state ──────────────────────────────────────────────── */
    c->running = 1;
    c->done = 0;
    c->error_code = 0;
    c->total_bytes = 0;
    c->start_time = get_timestamp_ms();
    c->first_token_time = 0;
    c->end_time = 0;
    c->state = CLIENT_STATE_CONNECTING;

    /* Clear parent buffer */
    c->main_buffer.len = 0;
    if (c->main_buffer.data) c->main_buffer.data[0] = '\0';

    /* Reap any previous zombie child */
    if (c->curl_pid > 0) {
        int status;
        waitpid(c->curl_pid, &status, WNOHANG);
        c->curl_pid = -1;
    }

    /* Close any existing pipe from previous request */
    if (c->data_pipe[0] >= 0) {
        fdclean(c->data_pipe[0]);
        close(c->data_pipe[0]);
        c->data_pipe[0] = -1;
    }
    if (c->data_pipe[1] >= 0) {
        close(c->data_pipe[1]);
        c->data_pipe[1] = -1;
    }
    yield();

    /* Build request body once (same for every retry) */
    char *body = build_request_body(c, messages);
    if (!body) {
        c->running = 0;
        return -1;
    }
    log_write(c, "Request body: %s", body);

    int attempt;
    int success = 0;

    /* ── Retry loop ───────────────────────────────────────────────── */
    for (attempt = 0; attempt <= c->max_retries; attempt++) {

        /* Cancellation check */
        if (!c->running) break;

        if (attempt > 0) {
            fprintf(stderr, "[retry %d/%d] waiting %dms...\n",
                    attempt, c->max_retries, c->retry_delay_ms);
            msleep(now() + c->retry_delay_ms);

            if (!c->running) break;  /* cancelled during sleep */

            /* Discard any error-response data from the failed attempt */
            c->main_buffer.len = 0;
            if (c->main_buffer.data) c->main_buffer.data[0] = '\0';
            c->total_bytes = 0;
        }

        /* ── Create pipe ───────────────────────────────────────── */
        if (pipe(c->data_pipe) != 0) break;
        int flags = fcntl(c->data_pipe[0], F_GETFL, 0);
        fcntl(c->data_pipe[0], F_SETFL, flags | O_NONBLOCK);

        /* ── Setup CURL ────────────────────────────────────────── */
        CURL *curl = curl_easy_init();
        if (!curl) {
            close(c->data_pipe[0]); close(c->data_pipe[1]);
            c->data_pipe[0] = -1; c->data_pipe[1] = -1;
            break;
        }

        curl_easy_setopt(curl, CURLOPT_URL, c->api_endpoint);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1800L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, client_curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);

        struct curl_slist *headers = NULL;
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->api_key);
        headers = curl_slist_append(headers, auth);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        /* ── Fork ──────────────────────────────────────────────── */
        pid_t pid = mfork();
        if (pid < 0) {
            log_write(c, "mfork failed: %s", strerror(errno));
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            close(c->data_pipe[0]); close(c->data_pipe[1]);
            c->data_pipe[0] = -1; c->data_pipe[1] = -1;
            break;
        }

        if (pid == 0) {
            /* ── CHILD: run curl, report error type via exit code ── */
            close(c->data_pipe[0]);
            log_write(c, "Child: curl perform (pid=%d, attempt=%d)",
                      getpid(), attempt);
            c->state = CLIENT_STATE_STREAMING;

            CURLcode res = curl_easy_perform(curl);
            int child_exit;

            if (res != CURLE_OK) {
                log_write(c, "Child: curl error: %s", curl_easy_strerror(res));
                child_exit = CHILD_EXIT_NET_ERR;
            } else {
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                log_write(c, "Child: HTTP %ld", http_code);
                if (http_code >= 200 && http_code < 300) {
                    child_exit = CHILD_EXIT_OK;
                } else if (http_code == 429) {
                    child_exit = CHILD_EXIT_HTTP_429;
                } else if (http_code >= 500) {
                    child_exit = CHILD_EXIT_HTTP_5XX;
                } else {
                    child_exit = CHILD_EXIT_HTTP_4XX;
                }
            }

            close(c->data_pipe[1]);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            log_write(c, "Child: _exit(%d)", child_exit);
            _exit(child_exit);
        }

        /* ── PARENT: peek at first data to confirm it is SSE ──────── */
        c->curl_pid = pid;
        close(c->data_pipe[1]);
        c->data_pipe[1] = -1;

        log_write(c, "Parent: peeking at pipe from child pid=%d", pid);

        /*
         * Read data until we either:
         *   a) See a "data:" prefix  → SSE stream confirmed, leave pipe
         *                               open so extract_chunk_internal can
         *                               continue reading in real time.
         *   b) Pipe EOF / hangup      → no valid SSE arrived; reap child,
         *                               check exit code, possibly retry.
         */
        int looks_sse = 0;
        while (c->running && !looks_sse) {
            int ev = fdwait(c->data_pipe[0], FDW_IN, now() + 100);

            if (ev & FDW_IN) {
                char buf[4096];
                ssize_t n = read(c->data_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    stream_buffer_append(&c->main_buffer, buf, n);
                    c->total_bytes += n;
                    if (buffer_looks_like_sse(&c->main_buffer)) {
                        looks_sse = 1;
                        break;
                    }
                } else if (n == 0) {
                    break;  /* EOF — no SSE seen */
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log_write(c, "read error: %s", strerror(errno));
                    break;
                }
            }
            if ((ev & FDW_ERR) && !(ev & FDW_IN)) {
                break;  /* hangup — no SSE seen */
            }
        }

        if (looks_sse) {
            /* SSE stream confirmed — leave pipe open for real-time streaming.
             * extract_chunk_internal will continue reading from the pipe. */
            c->state = CLIENT_STATE_STREAMING;
            success = 1;
            log_write(c, "Parent: SSE stream confirmed (attempt %d)", attempt);
            /* Parent's curl copy cleanup (child has its own) */
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            break;
        }

        /*
         * No SSE data arrived — the response was an error (HTTP 4xx/5xx,
         * network failure, etc.) or the pipe closed before any data.
         * Close the pipe, reap the child, and decide whether to retry.
         */
        fdclean(c->data_pipe[0]);
        close(c->data_pipe[0]);
        c->data_pipe[0] = -1;

        int status = 0;
        waitpid(pid, &status, 0);
        c->curl_pid = -1;
        c->done = 1;
        c->end_time = get_timestamp_ms();

        /* Clean up parent's curl copy */
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        /* ── Decide: retry or finish? ──────────────────────────── */
        if (WIFEXITED(status)) {
            int child_exit = WEXITSTATUS(status);

            int retryable = 0;
            const char *desc = "unknown";
            switch (child_exit) {
            case CHILD_EXIT_OK:
                /* HTTP 2xx but no SSE data?  Shouldn't happen — treat
                 * as non-retryable empty response. */
                desc = "empty response"; break;
            case CHILD_EXIT_NET_ERR:
                retryable = 1; desc = "network error"; break;
            case CHILD_EXIT_HTTP_5XX:
                retryable = 1; desc = "HTTP server error"; break;
            case CHILD_EXIT_HTTP_429:
                retryable = 1; desc = "HTTP 429 rate limit"; break;
            case CHILD_EXIT_HTTP_4XX:
                retryable = 0; desc = "HTTP client error"; break;
            }

            if (!retryable) {
                fprintf(stderr, "[error] %s (not retryable)\n", desc);
                c->state = CLIENT_STATE_ERROR;
                c->error_code = child_exit;
                break;
            }

            if (attempt >= c->max_retries) {
                fprintf(stderr, "[error] max retries (%d) exhausted — %s\n",
                        c->max_retries, desc);
                c->state = CLIENT_STATE_ERROR;
                c->error_code = child_exit;
                break;
            }

            fprintf(stderr, "[retry %d/%d] %s\n",
                    attempt + 1, c->max_retries, desc);

        } else {
            /* Child killed by signal — not retryable */
            log_write(c, "Parent: child killed by signal %d", WTERMSIG(status));
            c->state = CLIENT_STATE_ERROR;
            break;
        }
    }

    free(body);

    if (!success) {
        c->running = 0;
        return -1;
    }

    return 0;
}

int stream_client_has_more(stream_client_t *c) {
    if (!c) return 0;
    return !c->done || c->main_buffer.len > 0;
}

void stream_client_cancel(stream_client_t *c) {
    if (!c) return;
    log_write(c, "Cancelling stream");
    c->running = 0;
    
    /* Kill the child process running curl */
    if (c->curl_pid > 0) {
        log_write(c, "Killing child process (pid=%d)", c->curl_pid);
        kill(c->curl_pid, SIGKILL);
        /* Don't waitpid here — let wait_done or free reap the zombie */
    }
}

void stream_client_wait_done(stream_client_t *c) {
    if (!c) return;

    /* Wait for child process to exit and reap it */
    if (c->curl_pid > 0) {
        int status;
        log_write(c, "Waiting for child process (pid=%d)", c->curl_pid);
        waitpid(c->curl_pid, &status, 0);

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                c->state = CLIENT_STATE_DONE;
            } else {
                c->state = CLIENT_STATE_ERROR;
                c->error_code = exit_code;
                log_write(c, "Child exited with code %d", exit_code);
            }
        } else if (WIFSIGNALED(status)) {
            c->state = CLIENT_STATE_ERROR;
            log_write(c, "Child killed by signal %d", WTERMSIG(status));
        }

        c->curl_pid = -1;
        c->done = 1;
        c->end_time = get_timestamp_ms();
    }

    /* Close read end of pipe */
    if (c->data_pipe[0] >= 0) {
        fdclean(c->data_pipe[0]);
        close(c->data_pipe[0]);
        c->data_pipe[0] = -1;
    }
    if (c->data_pipe[1] >= 0) {
        close(c->data_pipe[1]);
        c->data_pipe[1] = -1;
    }
}

/* ============================================================================
 * Async Chunk Iteration (libmill)
 * ============================================================================ */

/*
 * Internal: Extract next SSE chunk with real-time pipe reading.
 * start_chat() confirmed the stream is valid SSE and left the pipe open;
 * we continue reading incrementally and parsing chunks as they arrive.
 * Returns: 1 = got chunk, 0 = no more data, -1 = error
 */
coroutine int extract_chunk_internal(stream_client_t *c, StreamChunk *chunk, int wait) {
    if (!c || !chunk) return -1;

    memset(chunk, 0, sizeof(StreamChunk));

    while (c->running) {
        /* Try to extract a chunk from the buffer first */
        int ret = extract_next_chunk(&c->main_buffer, chunk);
        if (ret == 1) {
            if (c->first_token_time == 0 &&
                (chunk->content || chunk->reasoning_content))
                c->first_token_time = get_timestamp_ms();
            return 1;
        } else if (ret == -1) {
            return -1;
        }

        /* No complete chunk yet */
        if (!wait) {
            return 0;
        }

        /* Wait for more data on the pipe */
        if (c->running) {
            int ev = fdwait(c->data_pipe[0], FDW_IN, now() + 100);

            /* Always drain the pipe first: FDW_ERR often accompanies FDW_IN
             * when the child process closes the pipe (EOF + hangup).
             * If we break on FDW_ERR before reading, we lose the last chunks. */
            if (ev & FDW_IN) {
                char buf[4096];
                ssize_t n = read(c->data_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    stream_buffer_append(&c->main_buffer, buf, n);
                    c->total_bytes += n;
                } else if (n == 0) {
                    /* Pipe closed — child process finished */
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log_write(c, "read from pipe error: %s", strerror(errno));
                    return -1;
                }
            }

            /* Only break on error if no data was readable (ev has FDW_ERR
             * but not FDW_IN — a real error, not just EOF notification). */
            if ((ev & FDW_ERR) && !(ev & FDW_IN)) {
                log_write(c, "fdwait error (no data), forcing pipe cleanup");
                fdclean(c->data_pipe[0]);
                break;
            }
            /* If fdwait timed out (ev == 0), loop back to check buffer again */
        }
    }

    /* Final attempt after pipe closed or cancelled */
    {
        int ret = extract_next_chunk(&c->main_buffer, chunk);
        return (ret == 1) ? 1 : 0;
    }
}

coroutine int next_chunk(stream_client_t *c, StreamChunk *chunk) {
    int ret = extract_chunk_internal(c, chunk, 1);
    return (ret == 1) ? 1 : 0;
}

coroutine int next_chunk_nowait(stream_client_t *c, StreamChunk *chunk) {
    int ret = extract_chunk_internal(c, chunk, 0);
    return (ret == 1) ? 1 : 0;
}

/* ============================================================================
 * Blocking API
 * ============================================================================ */
int stream_client_chat_blocking(stream_client_t *c, 
                                 const char *prompt,
                                 stream_chunk_callback_t callback,
                                 void *user_data) {
    if (!c || !prompt) return -1;
    
    /* Build a single-turn messages array from the prompt string */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON *messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, user_msg);
    
    int ret = stream_client_start_chat(c, messages);
    cJSON_Delete(messages);
    
    if (ret != 0) {
        return -1;
    }
    
    StreamChunk chunk;
    while (next_chunk(c, &chunk)) {
        if (callback) {
            callback(&chunk, user_data);
        }
        stream_chunk_cleanup(&chunk);
    }
    
    stream_client_wait_done(c);
    return (c->error_code == 0) ? 0 : -1;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */
client_state_t stream_client_get_state(stream_client_t *c) {
    if (!c) return CLIENT_STATE_ERROR;
    return c->state;
}

const char *stream_client_get_state_string(stream_client_t *c) {
    if (!c) return "null";
    switch (c->state) {
        case CLIENT_STATE_IDLE: return "idle";
        case CLIENT_STATE_CONNECTING: return "connecting";
        case CLIENT_STATE_STREAMING: return "streaming";
        case CLIENT_STATE_DONE: return "done";
        case CLIENT_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

int64_t stream_client_get_time_to_first_token(stream_client_t *c) {
    if (!c || c->first_token_time == 0) return -1;
    return c->first_token_time - c->start_time;
}

int64_t stream_client_get_total_duration(stream_client_t *c) {
    if (!c || c->start_time == 0) return -1;
    if (c->end_time > 0) {
        return c->end_time - c->start_time;
    }
    return get_timestamp_ms() - c->start_time;
}

size_t stream_client_get_total_bytes(stream_client_t *c) {
    if (!c) return 0;
    return c->total_bytes;
}

int stream_client_get_error_code(stream_client_t *c) {
    if (!c) return -1;
    return c->error_code;
}

const char *stream_client_get_error_message(stream_client_t *c) {
    if (!c) return NULL;
    if (c->error_message) return c->error_message;
    if (c->error_code != 0) {
        return curl_easy_strerror((CURLcode)c->error_code);
    }
    return NULL;
}
