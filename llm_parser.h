#ifndef LLM_PARSER_H
#define LLM_PARSER_H

#include "cjson/cJSON.h"
#include "openai_sse_parser.h"


/* ============================================================================
 * Parser Status / Return Codes
 * ============================================================================ */
typedef enum {
    /* Errors (negative) */
    LLM_PARSER_ERR_OOM    = -3,   /* memory allocation failed */
    LLM_PARSER_ERR_STATE  = -2,   /* state machine violation (e.g. delta without role) */
    LLM_PARSER_ERR_ARG    = -1,   /* invalid argument (NULL, invalid chunk, etc.) */

    /* Success states (non-negative) */
    LLM_PARSER_IDLE              = 0,   /* no active assistant message */
    LLM_PARSER_REASONING         = 1,   /* accumulating reasoning_content */
    LLM_PARSER_RESPONDING        = 2,   /* accumulating content */
    LLM_PARSER_WRITING_TOOL_CALL = 3,   /* accumulating tool_calls */
    LLM_PARSER_FINISHED          = 4,    /* this chunk triggered message completion */
    LLM_PARSER_ASSISTANT_START   = 5,
} LlmParserStatus;

/* ============================================================================
 * Opaque Handle
 * ============================================================================ */
typedef struct LlmParser LlmParser;

/* ============================================================================
 * Public API
 * ============================================================================ */
LlmParser *llm_parser_create(void);
void       llm_parser_destroy(LlmParser *p);

/* Add a complete user/tool/system message (cJSON object). Will be cloned, caller should free the json. Must be called in IDLE. */
LlmParserStatus llm_parser_add_message(LlmParser *p, const cJSON *msg_obj);

/* Feed one assistant StreamChunk. Returns current message status or negative error. */
LlmParserStatus llm_parser_feed_chunk(LlmParser *p, const StreamChunk *chunk);

/* Get the full history: {"messages":[...]}. Read-only; do not modify. */
const cJSON *llm_parser_get_history(const LlmParser *p);

/* Reset parser to initial empty state (drops everything). */
void llm_parser_reset(LlmParser *p);

/* Get last error string, or NULL if no error. */
const char *llm_parser_get_error(const LlmParser *p);

const char* llm_parser_status_to_str(LlmParserStatus status);

/* Force-finish the current assistant message (e.g., on cancellation).
 * Finalizes whatever content/reasoning has been accumulated so far.
 * Incomplete tool calls are discarded.
 * Returns LLM_PARSER_FINISHED if a message was finalized, LLM_PARSER_IDLE if none. */
LlmParserStatus llm_parser_force_finish(LlmParser *p);

/* Get usage from the last finished turn. Returns 0 on success, -1 if none. */
int llm_parser_get_last_usage(const LlmParser *p,
                              int *prompt, int *completion,
                              int *total, int *cached);

/*
 * Remove and free the last message from the history array.
 * Returns the number of remaining messages, or -1 if history is empty.
 */
int llm_parser_pop_last_message(LlmParser *p);

/*
 * Get a preview of in-progress tool calls. Writes to buf (max bufsz chars).
 * Returns empty string if no tool calls are being accumulated.
 */
const char *llm_parser_get_tool_preview(const LlmParser *p,
                                        char *buf, size_t bufsz);

#endif /* LLM_PARSER_H */