/*
 * openai_stream_client.c
 * 
 * Async streaming client for OpenAI-compatible APIs.
 * Lifecycle, configuration, SSE chunk iteration, blocking API, utilities.
 * Curl child-process management is delegated to stream_curl.c.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <curl/curl.h>

#include "openai_stream_client.h"
#include "stream_log.h"
#include "stream_curl.h"
#include "openai_sse_parser.h"
#include "libmill/libmill.h"

/* ============================================================================
 * Client Lifecycle
 * ============================================================================ */
stream_client_t *stream_client_new(const char *api_key, 
                                    const char *model,
                                    const char *api_endpoint, 
                                    const char *log_file,
                                    const char *system_message) {
    if (!api_key || !model) {
        return NULL;
    }
    
    stream_client_t *c = calloc(1, sizeof(stream_client_t));
    if (!c) return NULL;
    
    c->api_key = strdup(api_key);
    c->model = strdup(model);
    c->api_endpoint = strdup(api_endpoint ? api_endpoint : 
                              "https://api.moonshot.cn/v1/chat/completions");
    c->system_message = system_message ? strdup(system_message) : NULL;
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
    c->max_tokens = 0;
    
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

/* ============================================================================
 * Configuration
 * ============================================================================ */
void stream_client_set_system_message(stream_client_t *c, const char *message) {
    if (!c || !message) return;
    free(c->system_message);
    c->system_message = strdup(message);
}

void stream_client_set_temperature(stream_client_t *c, double temp) {
    if (!c) return;
    c->temperature = temp;
}

void stream_client_set_model(stream_client_t *c, const char *model) {
    if (!c || !model) return;
    free(c->model);
    c->model = strdup(model);
}

void stream_client_set_api(stream_client_t *c, const char *api_key, const char *api_endpoint) {
    if (!c) return;
    if (api_key) {
        free(c->api_key);
        c->api_key = strdup(api_key);
    }
    if (api_endpoint) {
        free(c->api_endpoint);
        c->api_endpoint = strdup(api_endpoint);
    }
}

const char *stream_client_get_model(stream_client_t *c) {
    return c ? c->model : NULL;
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

void stream_client_set_max_tokens(stream_client_t *c, int max_tokens) {
    if (!c) return;
    c->max_tokens = (max_tokens >= 0) ? max_tokens : 0;
}

/* ============================================================================
 * Streaming API (thin wrappers → stream_curl.c)
 * ============================================================================ */

coroutine int stream_client_start_chat(stream_client_t *c, cJSON *messages) {
    return stream_curl_start_chat(c, messages);
}

int stream_client_has_more(stream_client_t *c) {
    if (!c) return 0;
    return !c->done || c->main_buffer.len > 0;
}

void stream_client_cancel(stream_client_t *c) {
    stream_curl_cancel(c);
}

void stream_client_wait_done(stream_client_t *c) {
    stream_curl_wait_done(c);
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
coroutine static int extract_chunk_internal(stream_client_t *c, StreamChunk *chunk, int wait) {
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
            log_write(c, "extract_next_chunk returned -1 (error)");
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
                    log_write(c, "read %zd bytes from pipe (buf=%zu -> %zu)",
                             n, c->main_buffer.len, c->main_buffer.len + n);
                    stream_buffer_append(&c->main_buffer, buf, n);
                    c->total_bytes += n;
                } else if (n == 0) {
                    /* Pipe closed - child process finished */
                    log_write(c, "pipe EOF (child closed pipe)");
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log_write(c, "read from pipe error: %s", strerror(errno));
                    return -1;
                }
            }

            /* Only break on error if no data was readable (ev has FDW_ERR
             * but not FDW_IN - a real error, not just EOF notification). */
            if ((ev & FDW_ERR) && !(ev & FDW_IN)) {
                log_write(c, "fdwait error (no data), forcing pipe cleanup (buf_len=%zu)", c->main_buffer.len);
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
