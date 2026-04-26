/*
 * openai_sse_parser.c
 * 
 * SSE (Server-Sent Events) parser implementation.
 * Handles proper JSON validation and chunk extraction.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openai_sse_parser.h"

#define INITIAL_BUFFER_SIZE 65536
Role str_to_role(const char *str) {
    if (str == NULL) return ROLE_UNKNOWN;
    
    if (strcmp(str, "system") == 0)    return ROLE_SYSTEM;
    if (strcmp(str, "user") == 0)      return ROLE_USER;
    if (strcmp(str, "assistant") == 0) return ROLE_ASSISTANT;
    if (strcmp(str, "tool") == 0)      return ROLE_TOOL;
    
    return ROLE_UNKNOWN;
}
const char* role_to_str(Role role) {
    switch (role) {
        case ROLE_SYSTEM:    return "system";
        case ROLE_USER:      return "user";
        case ROLE_ASSISTANT: return "assistant";
        case ROLE_TOOL:      return "tool";
        default:             return "unknown";
    }
}
/* ============================================================================
 * Stream Buffer Functions
 * ============================================================================ */
int stream_buffer_init(stream_buffer_t *buf) {
    buf->capacity = INITIAL_BUFFER_SIZE;
    buf->data = malloc(buf->capacity);
    if (!buf->data) return -1;
    buf->len = 0;
    buf->data[0] = '\0';
    return 0;
}

void stream_buffer_free(stream_buffer_t *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

int stream_buffer_append(stream_buffer_t *buf, const char *data, size_t len) {
    if (buf->len + len + 1 > buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        while (new_capacity < buf->len + len + 1) {
            new_capacity *= 2;
        }
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) return -1;
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

void stream_buffer_consume(stream_buffer_t *buf, size_t len) {
    if (len >= buf->len) {
        buf->len = 0;
        if (buf->data) buf->data[0] = '\0';
    } else {
        memmove(buf->data, buf->data + len, buf->len - len + 1);
        buf->len -= len;
    }
}

/* ============================================================================
 * JSON Validation
 * ============================================================================ */
ssize_t find_json_end(const char *str, size_t len) {
    const char *parse_end = NULL;
    
    /* Try to parse with cJSON - the require_null_terminated flag is 0
     * because we want to know how much was parsed even if there's more data */
    cJSON *root = cJSON_ParseWithOpts(str, &parse_end, 0);
    
    if (root) {
        ssize_t consumed = (ssize_t)(parse_end - str);
        cJSON_Delete(root);
        return consumed;
    }
    
    return -1;  /* Invalid or incomplete JSON */
}

int is_valid_json(const char *str, size_t len) {
    ssize_t result = find_json_end(str, len);
    return (result > 0 && (size_t)result <= len);
}

/* ============================================================================
 * StreamChunk Helper Functions
 * ============================================================================ */
static char* safe_get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item))
        return strdup(item->valuestring);
    return NULL;
}

static int safe_get_int(cJSON *obj, const char *key, int default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item))
        return item->valueint;
    return default_val;
}

void stream_chunk_cleanup(StreamChunk *chunk) {
    if (!chunk) return;
    free(chunk->id);
    free(chunk->model);
    free(chunk->content);
    free(chunk->reasoning_content);
    if (chunk->tool_calls) cJSON_Delete(chunk->tool_calls);
    memset(chunk, 0, sizeof(StreamChunk));
}

int stream_chunk_copy(StreamChunk *dst, const StreamChunk *src) {
    if (!dst || !src) return -1;
    
    memset(dst, 0, sizeof(StreamChunk));
    
    dst->created = src->created;
    dst->finish_reason_present = src->finish_reason_present;
    dst->usage_present = src->usage_present;
    dst->prompt_tokens = src->prompt_tokens;
    dst->completion_tokens = src->completion_tokens;
    dst->total_tokens = src->total_tokens;
    dst->cached_tokens = src->cached_tokens;
    dst->is_done = src->is_done;
    dst->is_valid = src->is_valid;
    
    if (src->finish_reason_present) {
        strncpy(dst->finish_reason, src->finish_reason, sizeof(dst->finish_reason) - 1);
        dst->finish_reason[sizeof(dst->finish_reason) - 1] = '\0';
    }
    
    if (src->id) dst->id = strdup(src->id);
    if (src->model) dst->model = strdup(src->model);
    if (src->role) dst->role = src->role;
    if (src->content) dst->content = strdup(src->content);
    if (src->reasoning_content) dst->reasoning_content = strdup(src->reasoning_content);
    if (src->tool_calls) dst->tool_calls = cJSON_Duplicate(src->tool_calls, 1);
    
    return 0;
}

/* ============================================================================
 * StreamChunk Parsing
 * ============================================================================ */
int stream_chunk_parse(const char *json_str, StreamChunk *chunk) {
    cJSON *root = NULL;
    cJSON *choices = NULL;
    cJSON *choice0 = NULL;
    cJSON *delta = NULL;
    cJSON *usage = NULL;
    int ret = -1;
    
    memset(chunk, 0, sizeof(StreamChunk));
    chunk->cached_tokens = -1;
    
    /* Handle [DONE] marker */
    if (strcmp(json_str, "[DONE]") == 0) {
        chunk->is_done = 1;
        chunk->is_valid = 1;
        return 0;
    }
    
    root = cJSON_Parse(json_str);
    if (!root) {
        return -1;
    }
    
    /* Extract top-level fields */
    chunk->id = safe_get_string(root, "id");
    chunk->model = safe_get_string(root, "model");
    chunk->created = safe_get_int(root, "created", 0);
    
    /* Navigate to choices[0].delta */
    choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
        goto check_usage;
    
    choice0 = cJSON_GetArrayItem(choices, 0);
    if (!cJSON_IsObject(choice0))
        goto check_usage;
    
    /* Finish reason */
    cJSON *finish_reason = cJSON_GetObjectItem(choice0, "finish_reason");
    if (cJSON_IsString(finish_reason)) {
        chunk->finish_reason_present = 1;
        strncpy(chunk->finish_reason, finish_reason->valuestring, sizeof(chunk->finish_reason) - 1);
        chunk->finish_reason[sizeof(chunk->finish_reason) - 1] = '\0';
    }
    
    delta = cJSON_GetObjectItem(choice0, "delta");
    if (!cJSON_IsObject(delta))
        goto check_usage;
    
    /* Extract delta fields */
    chunk->role = str_to_role(safe_get_string(delta, "role"));
    chunk->content = safe_get_string(delta, "content");
    
    /* Reasoning content */
    cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");
    if (!cJSON_IsString(reasoning))
        reasoning = cJSON_GetObjectItem(delta, "thinking");
    if (cJSON_IsString(reasoning))
        chunk->reasoning_content = strdup(reasoning->valuestring);
    
    /* Tool calls */
    cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
    if (cJSON_IsArray(tool_calls)) {
        chunk->tool_calls = cJSON_Duplicate(tool_calls, 1);
    }
    
check_usage:
    /* Extract usage if present */
    if (cJSON_IsObject(choice0)) {
        usage = cJSON_GetObjectItem(choice0, "usage");
    }
    if (!cJSON_IsObject(usage)) {
        usage = cJSON_GetObjectItem(root, "usage");
    }
    
    if (cJSON_IsObject(usage)) {
        chunk->prompt_tokens = safe_get_int(usage, "prompt_tokens", -1);
        chunk->completion_tokens = safe_get_int(usage, "completion_tokens", -1);
        chunk->total_tokens = safe_get_int(usage, "total_tokens", -1);
        
        /* cached_tokens: try multiple formats */
        chunk->cached_tokens = -1;
        
        /* 1) DeepSeek-style: prompt_cache_hit_tokens at usage top level */
        if (chunk->cached_tokens < 0)
            chunk->cached_tokens = safe_get_int(usage, "prompt_cache_hit_tokens", -1);
        
        /* 2) OpenAI / Kimi-style: usage.prompt_tokens_details.cached_tokens */
        if (chunk->cached_tokens < 0) {
            cJSON *details = cJSON_GetObjectItem(usage, "prompt_tokens_details");
            if (cJSON_IsObject(details))
                chunk->cached_tokens = safe_get_int(details, "cached_tokens", -1);
        }
        
        /* 3) Direct flat key (legacy / some providers) */
        if (chunk->cached_tokens < 0)
            chunk->cached_tokens = safe_get_int(usage, "cached_tokens", -1);
        
        /* 4) If still missing, default to 0 */
        if (chunk->cached_tokens < 0)
            chunk->cached_tokens = 0;
        
        chunk->usage_present = 1;
    }
    
    chunk->is_valid = 1;
    ret = 0;
    
    if (root) cJSON_Delete(root);
    return ret;
}

/* ============================================================================
 * SSE Event Extraction
 * ============================================================================ */
char *extract_sse_event(stream_buffer_t *buf) {
    /* Look for double newline as SSE event separator */
    size_t sep_pos = 0;
    int found_sep = 0;
    int sep_len = 0;
    
    for (size_t i = 0; i + 1 < buf->len; i++) {
        if (buf->data[i] == '\n' && buf->data[i + 1] == '\n') {
            sep_pos = i;
            found_sep = 1;
            sep_len = 2;
            break;
        }
        if (i + 3 < buf->len && 
            buf->data[i] == '\r' && buf->data[i + 1] == '\n' &&
            buf->data[i + 2] == '\r' && buf->data[i + 3] == '\n') {
            sep_pos = i;
            found_sep = 1;
            sep_len = 4;
            break;
        }
    }
    
    if (!found_sep) {
        return NULL;  /* No complete event yet */
    }
    
    /* Calculate event length */
    size_t event_len = sep_pos;
    
    /* Skip leading whitespace/newlines */
    size_t start = 0;
    while (start < event_len && (buf->data[start] == '\n' || buf->data[start] == '\r' ||
                                  buf->data[start] == ' ' || buf->data[start] == '\t')) {
        start++;
    }
    
    /* Skip "data: " prefix if present */
    if (start + 6 <= event_len && strncmp(buf->data + start, "data: ", 6) == 0) {
        start += 6;
    }
    
    /* Allocate and copy the event data */
    size_t content_len = event_len > start ? event_len - start : 0;
    
    char *event = malloc(content_len + 1);
    if (!event) return NULL;
    
    if (content_len > 0) {
        memcpy(event, buf->data + start, content_len);
    }
    event[content_len] = '\0';
    
    /* Consume the event from buffer (including separator) */
    stream_buffer_consume(buf, sep_pos + sep_len);
    
    return event;
}

char *extract_valid_json(stream_buffer_t *buf) {
    /* Skip leading whitespace */
    size_t start = 0;
    while (start < buf->len && (buf->data[start] == ' ' || buf->data[start] == '\t' ||
                                 buf->data[start] == '\n' || buf->data[start] == '\r')) {
        start++;
    }
    
    if (start >= buf->len) {
        stream_buffer_consume(buf, start);
        return NULL;
    }
    
    /* JSON must start with { or [ */
    if (buf->data[start] != '{' && buf->data[start] != '[') {
        /* Not JSON, consume and return */
        stream_buffer_consume(buf, start + 1);
        return NULL;
    }
    
    /* Try to find valid JSON starting at 'start' */
    ssize_t json_len = find_json_end(buf->data + start, buf->len - start);
    
    if (json_len < 0) {
        /* No valid JSON found - could be incomplete */
        return NULL;
    }
    
    /* Valid JSON found, extract it */
    char *json = malloc(json_len + 1);
    if (!json) return NULL;
    
    memcpy(json, buf->data + start, json_len);
    json[json_len] = '\0';
    
    /* Consume from buffer */
    stream_buffer_consume(buf, start + json_len);
    
    return json;
}

/* ============================================================================
 * Main Extraction Logic
 * ============================================================================ */
int extract_next_chunk(stream_buffer_t *buf, StreamChunk *chunk) {
    if (!buf || !chunk) return -1;
    
    memset(chunk, 0, sizeof(StreamChunk));
    
    /* Skip leading whitespace and empty lines */
    size_t start = 0;
    while (start < buf->len && (buf->data[start] == '\n' || buf->data[start] == '\r' ||
                                 buf->data[start] == ' ' || buf->data[start] == '\t')) {
        start++;
    }
    
    if (start > 0) {
        stream_buffer_consume(buf, start);
    }
    
    if (buf->len == 0) {
        return 0;  /* No data */
    }
    
    /* Check for [DONE] marker */
    if (strncmp(buf->data, "[DONE]", 6) == 0 ||
        strncmp(buf->data, "data: [DONE]", 12) == 0) {
        chunk->is_done = 1;
        chunk->is_valid = 1;
        /* Consume the marker */
        if (strncmp(buf->data, "data: [DONE]", 12) == 0) {
            stream_buffer_consume(buf, 12);
            /* Also consume following newlines */
            while (buf->len > 0 && (buf->data[0] == '\n' || buf->data[0] == '\r')) {
                stream_buffer_consume(buf, 1);
            }
        } else {
            stream_buffer_consume(buf, 6);
        }
        return 1;
    }
    
    /* Check for "data: " prefix */
    if (strncmp(buf->data, "data: ", 6) == 0) {
        /* Try to find the end of this data line/event */
        size_t line_end = 6;
        int multi_line = 0;
        
        /* First try: look for newline */
        while (line_end < buf->len && buf->data[line_end] != '\n' && 
               buf->data[line_end] != '\r') {
            line_end++;
        }
        
        /* Check if we have a complete line */
        if (line_end >= buf->len) {
            /* Incomplete line, need more data */
            return 0;
        }
        
        /* Extract content and validate as JSON */
        size_t content_len = line_end - 6;
        
        /* Handle case where JSON might span multiple lines or contain "data:" */
        char *content = malloc(content_len + 1);
        if (!content) return -1;
        
        memcpy(content, buf->data + 6, content_len);
        content[content_len] = '\0';
        
        ssize_t valid_len = find_json_end(content, content_len);
        
        if (valid_len > 0 && (size_t)valid_len == content_len) {
            /* Valid JSON on single line */
            int ret = stream_chunk_parse(content, chunk);
            free(content);
            
            /* Consume line and following newlines */
            size_t consume_len = line_end;
            while (consume_len < buf->len && 
                   (buf->data[consume_len] == '\n' || buf->data[consume_len] == '\r')) {
                consume_len++;
            }
            stream_buffer_consume(buf, consume_len);
            
            return (ret == 0) ? 1 : -1;
        }
        
        /* JSON might be multi-line or incomplete, try event-based extraction */
        free(content);
        
        /* Look for next "data:" or double newline as event boundary */
        size_t event_end = 6;
        while (event_end < buf->len) {
            /* Check for next "data: " which indicates a new event */
            if (event_end + 8 <= buf->len) {
                if (strncmp(buf->data + event_end, "\ndata: ", 8) == 0 ||
                    strncmp(buf->data + event_end, "\r\ndata: ", 9) == 0) {
                    break;
                }
            }
            /* Check for event separator */
            if (event_end + 2 <= buf->len) {
                if (strncmp(buf->data + event_end, "\n\n", 2) == 0 ||
                    strncmp(buf->data + event_end, "\r\n\r\n", 4) == 0) {
                    break;
                }
            }
            event_end++;
        }
        
        if (event_end < buf->len) {
            /* We have a potential complete event */
            size_t json_start = 6;
            size_t json_len_calc = event_end - json_start;
            
            /* Trim trailing whitespace */
            while (json_len_calc > 0 && 
                   (buf->data[json_start + json_len_calc - 1] == '\n' ||
                    buf->data[json_start + json_len_calc - 1] == '\r' ||
                    buf->data[json_start + json_len_calc - 1] == ' ' ||
                    buf->data[json_start + json_len_calc - 1] == '\t')) {
                json_len_calc--;
            }
            
            if (json_len_calc > 0) {
                char *json_str = malloc(json_len_calc + 1);
                if (!json_str) return -1;
                
                memcpy(json_str, buf->data + json_start, json_len_calc);
                json_str[json_len_calc] = '\0';
                
                /* Validate the JSON */
                ssize_t valid_json_len = find_json_end(json_str, json_len_calc);
                
                if (valid_json_len > 0 && (size_t)valid_json_len == (ssize_t)json_len_calc) {
                    /* Valid complete JSON */
                    int ret = stream_chunk_parse(json_str, chunk);
                    free(json_str);
                    
                    /* Consume up to event separator */
                    size_t consume_len = event_end;
                    /* Include the separator itself */
                    if (event_end + 2 <= buf->len && strncmp(buf->data + event_end, "\n\n", 2) == 0) {
                        consume_len += 2;
                    } else if (event_end + 4 <= buf->len && strncmp(buf->data + event_end, "\r\n\r\n", 4) == 0) {
                        consume_len += 4;
                    }
                    stream_buffer_consume(buf, consume_len);
                    
                    return (ret == 0) ? 1 : -1;
                }
                
                free(json_str);
            }
            
            /* Invalid JSON, consume "data: " and try again */
            stream_buffer_consume(buf, 6);
            return 0;
        }
        
        /* Incomplete event, need more data */
        return 0;
    }
    
    /* No "data: " prefix - try raw JSON extraction */
    if (buf->data[0] == '{') {
        char *json = extract_valid_json(buf);
        if (json) {
            int ret = stream_chunk_parse(json, chunk);
            free(json);
            return (ret == 0) ? 1 : -1;
        }
        /* Need more data for valid JSON */
        return 0;
    }
    
    /* Unknown format, skip one character */
    stream_buffer_consume(buf, 1);
    return 0;
}
