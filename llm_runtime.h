/*
 * llm_runtime.h
 *
 * High-level LLM runtime that orchestrates the stream client, message history
 * parser, and tool execution into a coherent multi-turn conversation loop.
 *
 * Usage (in a libmill coroutine):
 *
 *   llm_runtime_t *rt = llm_runtime_new(api_key, model, endpoint, logfile);
 *   llm_runtime_register_tool(rt, "sleep", tool_sleep);
 *
 *   // In a coroutine:
 *   go(chat_session(rt));
 *
 *   coroutine void chat_session(llm_runtime_t *rt) {
 *       llm_runtime_send(rt, "Hello!", my_callback, NULL);
 *       llm_runtime_send(rt, "Tell me more", my_callback, NULL);
 *       // ...
 *   }
 *
 *   // Callback receives streaming events:
 *   void my_callback(llm_runtime_t *rt, llm_runtime_event_t ev,
 *                    const char *text, const cJSON *data, void *user) {
 *       if (ev == LLM_RT_EVENT_CONTENT) printf("%s", text);
 *   }
 */

#ifndef LLM_RUNTIME_H
#define LLM_RUNTIME_H

#include "cjson/cJSON.h"
#include "openai_stream_client.h"
#include "llm_parser.h"
#include "libmill/libmill.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Limits
 * ============================================================================ */
#define LLM_RUNTIME_MAX_TOOLS      64
#define LLM_RUNTIME_MAX_TOOL_LOOPS 16

/* ============================================================================
 * Event Types (sent to user callback)
 * ============================================================================ */
typedef enum {
    LLM_RT_EVENT_REASONING,      /* reasoning_content delta text          */
    LLM_RT_EVENT_CONTENT,        /* content delta text                    */
    LLM_RT_EVENT_TOOL_CALLS,     /* tool calls ready for execution        */
                                 /*   data = cJSON array of tool_call obj */
    LLM_RT_EVENT_TOOL_RESULT,    /* a single tool finished                */
                                 /*   text = tool name                    */
    LLM_RT_EVENT_USAGE,          /* token usage from the turn             */
                                 /*   data = {prompt,completion,total}    */
    LLM_RT_EVENT_DONE,           /* entire turn fully complete            */
    LLM_RT_EVENT_ERROR,          /* error occurred                        */
                                 /*   text = error message                */
    LLM_RT_EVENT_STATUS_CHANGE   /* parser state changed                  */
                                 /*   text = status name                  */
} llm_runtime_event_t;

/* ============================================================================
 * Opaque Handle (forward declaration)
 * ============================================================================ */
typedef struct llm_runtime llm_runtime_t;

/* ============================================================================
 * Tool Function Signature
 * ============================================================================ */

/*
 * A tool function receives the runtime handle (for cancellation checks, context
 * access), parsed JSON arguments, and returns a JSON result.
 *
 *   rt   – runtime handle. Can check llm_runtime_is_cancelled(rt) or access
 *          conversation history via llm_runtime_get_history(rt).
 *   args – parsed JSON arguments object. NULL if parsing failed.
 *
 * The result must be a heap-allocated cJSON value; the runtime will delete it.
 */
typedef cJSON *(*llm_tool_fn_t)(llm_runtime_t *rt, const cJSON *args);

/* ============================================================================
 * User Callback Signature
 * ============================================================================ */

/*
 * on_chunk callback. Called for each event during llm_runtime_send().
 *   rt       – the runtime handle
 *   event    – type of event
 *   text     – string payload (may be NULL for non-text events)
 *   data     – cJSON payload (may be NULL). Read-only; do not free.
 *   user_data – opaque pointer from llm_runtime_send()
 */
typedef void (*llm_runtime_callback_t)(llm_runtime_t *rt,
                                       llm_runtime_event_t event,
                                       const char *text,
                                       const cJSON *data,
                                       void *user_data);

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create a new LLM runtime.
 * All string parameters are copied internally.
 *   api_key      – required
 *   model        – required
 *   api_endpoint – NULL for default (Moonshot)
 *   log_file     – NULL for no logging
 */
llm_runtime_t *llm_runtime_new(const char *api_key, const char *model,
                                const char *api_endpoint, const char *log_file);

/* Destroy runtime, cancel any active request, free all memory. */
void llm_runtime_free(llm_runtime_t *rt);

/* ============================================================================
 * Configuration
 * ============================================================================ */
void llm_runtime_set_system_message(llm_runtime_t *rt, const char *msg);
void llm_runtime_set_temperature(llm_runtime_t *rt, double temp);

/* ============================================================================
 * Tool Registration
 * ============================================================================ */

/* Register a tool handler. name must match the LLM's function name.
 * Returns 0 on success, -1 on error (duplicate, max tools reached, etc.). */
int llm_runtime_register_tool(llm_runtime_t *rt, const char *name,
                               llm_tool_fn_t fn);

/*
 * Set the tool schemas to include in every request body.
 *
 * schemas: cJSON Array of tool definition objects, exactly as the API expects:
 *   [{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}]
 *
 * The array is deep-copied internally; caller retains ownership.
 * Pass NULL to clear all tool schemas.
 *
 * Note: This is separate from llm_runtime_register_tool(). register_tool
 *       sets which C function handles a named tool at runtime, while
 *       set_tool_schema tells the LLM which tools exist and their signatures.
 *       Both must be called for tools to work end-to-end.
 */
void llm_runtime_set_tool_schema(llm_runtime_t *rt, const cJSON *schemas);

/* ============================================================================
 * Conversation
 * ============================================================================ */

/*
 * Add a user message to history without sending.
 * The message is stored in the internal parser history.
 * Returns 0 on success, -1 on error.
 */
int llm_runtime_add_user_message(llm_runtime_t *rt, const char *text);

/* ============================================================================
 * Streaming Send (coroutine)
 * ============================================================================ */

/*
 * Send a user message and stream the response (coroutine).
 *
 * Must be called from a libmill coroutine (via go() or from another coroutine).
 * Blocks until the entire turn is complete, including any tool call loops.
 *
 *   user_text – user message text. If NULL, uses messages already added with
 *               llm_runtime_add_user_message().
 *   on_chunk  – callback for streaming events (may be NULL).
 *   user_data – opaque pointer passed to on_chunk.
 *
 * Returns 0 on success, -1 on error (call llm_runtime_get_error() for details).
 */
coroutine int llm_runtime_send(llm_runtime_t *rt,
                                const char *user_text,
                                llm_runtime_callback_t on_chunk,
                                void *user_data);

/* ============================================================================
 * Utilities
 * ============================================================================ */

/* Get conversation history (read-only). Format: {"messages": [...]} */
const cJSON *llm_runtime_get_history(const llm_runtime_t *rt);

/* Reset conversation (clear history, keep tools and config). */
void llm_runtime_reset(llm_runtime_t *rt);

/* Cancel any ongoing streaming request. */
void llm_runtime_cancel(llm_runtime_t *rt);

/* Check if cancellation was requested. Useful in long-running tool functions. */
int llm_runtime_is_cancelled(const llm_runtime_t *rt);

/* Get the last error message, or NULL if no error. */
const char *llm_runtime_get_error(const llm_runtime_t *rt);

/* Get current state as a string (from underlying stream client). */
const char *llm_runtime_get_state_string(const llm_runtime_t *rt);

#ifdef __cplusplus
}
#endif

#endif /* LLM_RUNTIME_H */
