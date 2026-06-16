/*
 * llm_runtime_internal.h
 *
 * Internal header shared between llm_runtime.c and agent_loop.c.
 * Contains the full llm_runtime struct definition and shared helpers.
 * NOT part of the public API - do not install.
 */

#ifndef LLM_RUNTIME_INTERNAL_H
#define LLM_RUNTIME_INTERNAL_H

#include "llm_runtime.h"
#include "sds/sds.h"
#include "cjson_arena.h"
#include <stdarg.h>

/* ============================================================================
 * Internal Structure (shared between llm_runtime.c & agent_loop.c)
 * ============================================================================ */
struct llm_runtime {
    /* Core components */
    stream_client_t *client;
    LlmParser *parser;

    /* Cached config strings (also stored inside stream_client) */
    Arena arena;
    sds api_key;
    sds model;
    sds api_endpoint;
    sds log_file;

    /* Registered tool handlers */
    struct {
        char name[128];
        llm_tool_fn_t fn;
    } tools[LLM_RUNTIME_MAX_TOOLS];
    int tool_count;

    /* Error state */
    char error_msg[256];
    int has_error;

    /* Cancellation flag */
    volatile int running;

    /* Pointer to cop_context_t.running_child_pid (set by cop.c).
     * llm_runtime_popen writes the child PID here so the signal handler
     * in cop_ui.c can kill it on Ctrl+C. */
    volatile pid_t *child_pid_ptr;

    /* YOLO mode: auto-approve tool calls */
    int yolo;

    /* Usage statistics from the most recent step */
    int     usage_seen;
    int     usage_prompt;
    int     usage_completion;
    int     usage_total;
    int     usage_cached;
    int64_t usage_elapsed_ms;
};

/* ============================================================================
 * Shared Helpers
 * ============================================================================ */

static inline void set_error(llm_runtime_t *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->error_msg, sizeof(rt->error_msg), fmt, ap);
    va_end(ap);
    rt->has_error = 1;
}

#endif /* LLM_RUNTIME_INTERNAL_H */
