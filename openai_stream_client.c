/*
 * openai_stream_client.c
 * 
 * Async streaming client for OpenAI-compatible APIs.
 * Uses libcurl in a separate thread with libmill coroutines for async iteration.
 * 
 * Thread Communication:
 * - CURL thread writes to buffer, then writes byte to pipe to notify main thread
 * - Main thread uses fdwait() on pipe to wait for data
 * - Buffer is protected by mutex
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
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
 * JSON Escaping
 * ============================================================================ */
static void json_escape(const char *input, char *output, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j < out_size - 1; i++) {
        switch (input[i]) {
            case '"': 
                if (j + 2 < out_size) { output[j++] = '\\'; output[j++] = '"'; }
                break;
            case '\\':
                if (j + 2 < out_size) { output[j++] = '\\'; output[j++] = '\\'; }
                break;
            case '\n':
                if (j + 2 < out_size) { output[j++] = '\\'; output[j++] = 'n'; }
                break;
            case '\r':
                if (j + 2 < out_size) { output[j++] = '\\'; output[j++] = 'r'; }
                break;
            case '\t':
                if (j + 2 < out_size) { output[j++] = '\\'; output[j++] = 't'; }
                break;
            default:
                output[j++] = input[i];
        }
    }
    output[j] = '\0';
}

/* ============================================================================
 * CURL Callbacks
 * ============================================================================ */
typedef struct {
    stream_client_t *client;
    CURL *curl;
    char *body;
} curl_thread_data_t;

static size_t client_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    stream_client_t *c = (stream_client_t *)userdata;
    
    if (!c->running) {
        log_write(c, "CURL write callback: aborting (running=0)");
        return 0;
    }
    
    c->total_bytes += total;
    log_raw_data(c, ptr, total);
    
    /* Write raw data to the pipe. If the pipe is full, this blocks until
       the main thread reads data. That provides backpressure. */
    ssize_t written = write(c->data_pipe[1], ptr, total);
    if (written != (ssize_t)total) {
        /* EPIPE means main thread closed the pipe (cancellation) */
        if (errno == EPIPE) {
            log_write(c, "Pipe closed by main thread (cancelled)");
        } else {
            log_write(c, "Failed to write all data to pipe (errno=%d)", errno);
        }
        return 0;   /* signal error to CURL */
    }
    
    return total;
}

static int client_curl_xfer_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                               curl_off_t ultotal, curl_off_t ulnow) {
    stream_client_t *c = (stream_client_t *)clientp;
    
    /* Return non-zero to abort transfer if running is 0 */
    if (!c->running) {
        log_write(c, "CURL xfer callback: aborting (running=0)");
        return 1;  /* Abort */
    }
    
    return 0;  /* Continue */
}

/* ============================================================================
 * CURL Thread
 * ============================================================================ */
static void *curl_thread_func(void *arg) {
    curl_thread_data_t *td = (curl_thread_data_t *)arg;
    stream_client_t *c = td->client;
    CURL *curl = td->curl;
    
    log_write(c, "CURL thread started");
    c->curl_running = 1;
    c->state = CLIENT_STATE_STREAMING;
    
    struct curl_slist *headers = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", c->api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, client_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, client_curl_xfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, c);
    
    log_write(c, "Starting CURL perform");
    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        if (!c->running && res == CURLE_WRITE_ERROR) {
            /* This is expected when we abort */
            log_write(c, "CURL aborted by user");
        } else {
            log_write(c, "CURL error: %s", curl_easy_strerror(res));
            c->error_code = res;
            c->state = CLIENT_STATE_ERROR;
        }
    } else {
        log_write(c, "CURL completed successfully");
        c->state = CLIENT_STATE_DONE;
    }
    
    /* Mark as done - this signals the main thread to finish */
    c->curl_running = 0;
    c->done = 1;
    c->end_time = get_timestamp_ms();
    
    /* After CURL finishes, close the write end of the pipe to signal EOF */
    if (c->data_pipe[1] >= 0) {
        log_write(c, "%s:%d,closing write pipe",__FILE__,__LINE__);
        close(c->data_pipe[1]);
        c->data_pipe[1] = -1;
    }
    
    free(td->body);
    free(td);
    
    log_write(c, "CURL thread exiting");
    return NULL;
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
    c->curl_running = 0;
    c->done = 0;
    c->error_code = 0;
    c->data_pipe[0] = -1;
    c->data_pipe[1] = -1;
    
    /* Initialize main thread buffer */
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
    
    /* Cancel any ongoing request */
    stream_client_cancel(c);
    
    /* Wait for CURL thread to finish */
    if (c->curl_running) {
        pthread_join(c->curl_thread, NULL);
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
static int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
    if (type == CURLINFO_TEXT || type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT) {
        fwrite(data, 1, size, stdout);
    }
    return 0;
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
int stream_client_start_chat(stream_client_t *c, cJSON *messages) {
    if (!c) return -1;
    
    /* Reset state */
    c->running = 1;
    c->done = 0;
    c->error_code = 0;
    c->total_bytes = 0;
    c->start_time = get_timestamp_ms();
    c->first_token_time = 0;
    c->end_time = 0;
    c->state = CLIENT_STATE_CONNECTING;
    
    /* Clear main buffer (only accessed by main thread) */
    c->main_buffer.len = 0;
    if (c->main_buffer.data) c->main_buffer.data[0] = '\0';
    
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
    
    /* Create new data pipe for this request */
    if (pipe(c->data_pipe) != 0) {
        c->running = 0;
        return -1;
    }
    
    /* Make read end non-blocking for fdwait */
    int flags = fcntl(c->data_pipe[0], F_GETFL, 0);
    fcntl(c->data_pipe[0], F_SETFL, flags | O_NONBLOCK);
    
    /* Build request body from client config + messages */
    char *body = build_request_body(c, messages);
    if (!body) {
        c->running = 0;
        return -1;
    }
    
    log_write(c, "Request body: %s", body);
    
    /* Setup CURL */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        c->running = 0;
        return -1;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, c->api_endpoint);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    
    curl_thread_data_t *td = malloc(sizeof(curl_thread_data_t));
    if (!td) {
        curl_easy_cleanup(curl);
        free(body);
        c->running = 0;
        return -1;
    }
    td->client = c;
    td->curl = curl;
    td->body = body;
    
    /* Start CURL thread */
    if (pthread_create(&c->curl_thread, NULL, curl_thread_func, td) != 0) {
        curl_easy_cleanup(curl);
        free(body);
        free(td);
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
    /* Note: We do NOT close the pipe here because fdwait() may be active
       in another coroutine. Closing a fd while fdwait is watching it
       causes epoll errors. The fdwait has a 100ms timeout so it will
       wake up quickly anyway. */
}

void stream_client_wait_done(stream_client_t *c) {
    if (!c) return;
    if (c->curl_running) {
        pthread_join(c->curl_thread, NULL);
    }
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
 * Internal: Wait for data and extract next chunk.
 * Returns: 1 = got chunk, 0 = no more data, -1 = error
 */
coroutine int extract_chunk_internal(stream_client_t *c, StreamChunk *chunk, int wait) {
    if (!c || !chunk) return -1;
    
    memset(chunk, 0, sizeof(StreamChunk));
    
    int first_attempt = 1;
    int wait_for_curl = 0;
    
    /* Wait for CURL thread to start (max 5 seconds) */
    while (!c->curl_running && c->running && wait_for_curl < 50) {
        msleep(now() + 100);
        wait_for_curl++;
    }
    
    while (c->running && (c->curl_running || first_attempt)) {
        /* Try to extract a chunk from the main buffer first */
        int ret = extract_next_chunk(&c->main_buffer, chunk);
        
        if (ret == 1) {
            if (c->first_token_time == 0 && (chunk->content || chunk->reasoning_content))
                c->first_token_time = get_timestamp_ms();
            return 1;
        } else if (ret == -1) {
            return -1;
        }
        
        /* No complete chunk yet */
        first_attempt = 0;
        
        if (!c->curl_running && c->main_buffer.len == 0) {
            /* CURL done and buffer empty */
            break;
        }
        
        if (!wait) {
            /* Non-blocking mode */
            return 0;
        }
        
        /* Wait for data on the pipe */
        if (c->running && c->curl_running) {
            int ev = fdwait(c->data_pipe[0], FDW_IN, now() + 100);
            if (ev & FDW_ERR) {
                log_write(c, "fdwait error, forcing pipe cleanup");
                fdclean(c->data_pipe[0]);   
                break;
            }
            
            if (ev & FDW_IN) {
                /* Read available data into main buffer */
                char buf[4096];
                ssize_t n = read(c->data_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    stream_buffer_append(&c->main_buffer, buf, n);
                } else if (n == 0) {
                    /* Pipe closed - CURL thread is done */
                    break;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log_write(c, "read from pipe error: %s", strerror(errno));
                    return -1;
                }
            }
        }
    }
    
    /* Final attempt after CURL thread finished */
    int ret = extract_next_chunk(&c->main_buffer, chunk);
    return (ret == 1) ? 1 : 0;
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
