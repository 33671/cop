/*
 * openai_sse_parser.h
 * 
 * SSE (Server-Sent Events) parser for OpenAI-compatible streaming APIs.
 * Handles proper JSON validation and chunk extraction.
 */

#ifndef OPENAI_SSE_PARSER_H
#define OPENAI_SSE_PARSER_H

#include <stddef.h>
#include <stdio.h>
#include "cjson/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <stdio.h>
#include <string.h>

typedef enum {
    ROLE_SYSTEM = 0, // only used in the request
    ROLE_USER = 1, // only used in the request
    ROLE_ASSISTANT = 2,  // only from the response
    ROLE_TOOL = 3, // only used in the request
    ROLE_UNKNOWN = -1 
} Role;

Role str_to_role(const char *str);
const char* role_to_str(Role role);

/* ============================================================================
 * StreamChunk - Parsed SSE Data Structure
 * ============================================================================ */
typedef struct {
    /* Basic metadata */
    char *id;                /* response id (may be NULL) */
    char *model;             /* model name (may be NULL) */
    long created;            /* timestamp (0 if missing) */

    /* Delta content (all nullable) */
    Role role;              /* ROLE_ASSISTANT, usually only not Unknown on first chunk */
    char *content;           /* normal text content, NULL when reasoning*/
    char *reasoning_content; /* thinking/reasoning text (OpenAI) */
    int finish_reason_present; /* 1 if finish_reason exists */
    char finish_reason[32];

    /* Tool calls (array of cJSON objects) */
    cJSON *tool_calls;       /* cJSON_Array, may be NULL */

    /* Usage (nullable) */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cached_tokens;       /* -1 indicate this does not exist */
    int usage_present;       /* 1 if usage data was found in this chunk */
    
    /* Special markers */
    int is_done;             /* 1 if this is [DONE] marker */
    int is_valid;            /* 1 if this chunk contains valid data */
} StreamChunk;

/* ============================================================================
 * StreamBuffer - Accumulates Incomplete Data
 * ============================================================================ */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} stream_buffer_t;

/* Initialize/free stream buffer */
int stream_buffer_init(stream_buffer_t *buf);
void stream_buffer_free(stream_buffer_t *buf);
int stream_buffer_append(stream_buffer_t *buf, const char *data, size_t len);
void stream_buffer_consume(stream_buffer_t *buf, size_t len);

/* ============================================================================
 * JSON Validation
 * ============================================================================ */

/*
 * Validate JSON and return the number of bytes consumed.
 * Returns -1 if JSON is invalid or incomplete.
 * This handles the case where "data: " may appear inside JSON content.
 */
ssize_t find_json_end(const char *str, size_t len);

/*
 * Check if a string is valid complete JSON.
 * Returns 1 if valid, 0 otherwise.
 */
int is_valid_json(const char *str, size_t len);

/* ============================================================================
 * StreamChunk Parsing
 * ============================================================================ */

/*
 * Parse a JSON string into a StreamChunk.
 * Returns 0 on success, -1 on error.
 * Caller must call stream_chunk_cleanup() to free allocated memory.
 */
int stream_chunk_parse(const char *json_str, StreamChunk *chunk);

/*
 * Free all memory allocated inside a StreamChunk.
 */
void stream_chunk_cleanup(StreamChunk *chunk);

/*
 * Deep copy a StreamChunk.
 * Returns 0 on success, -1 on error.
 * Caller must call stream_chunk_cleanup() on the destination.
 */
int stream_chunk_copy(StreamChunk *dst, const StreamChunk *src);

/* ============================================================================
 * SSE Event Extraction
 * ============================================================================ */

/*
 * Extract a complete SSE event from the buffer.
 * SSE events are separated by double newlines (\n\n or \r\n\r\n).
 * Returns a newly allocated string containing the event content (after "data: "),
 * or NULL if no complete event is available.
 * The caller must free the returned string.
 */
char *extract_sse_event(stream_buffer_t *buf);

/*
 * Extract a complete JSON object from the buffer.
 * Uses cJSON_ParseWithOpts to validate JSON completeness.
 * Returns a newly allocated string containing the valid JSON, or NULL.
 * The caller must free the returned string.
 */
char *extract_valid_json(stream_buffer_t *buf);

/*
 * Process buffer and extract a single chunk.
 * Returns 1 if a chunk was extracted, 0 if no complete chunk available, -1 on error.
 * The 'chunk' output parameter is populated if return value is 1.
 */
int extract_next_chunk(stream_buffer_t *buf, StreamChunk *chunk);

#ifdef __cplusplus
}
#endif

#endif /* OPENAI_SSE_PARSER_H */
