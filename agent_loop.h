/*
 * agent_loop.h
 *
 * Internal header for the agent turn loop implementation.
 * NOT part of the public API — do not install.
 *
 * agent_loop_run() contains the core logic formerly in llm_runtime_send():
 * adding user messages, streaming, tool call detection and execution.
 */

#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "llm_runtime.h"

/*
 * agent_loop_run — execute one complete agent turn.
 *
 * This is the implementation formerly in llm_runtime_send().
 * Called by the public llm_runtime_send() wrapper in llm_runtime.c.
 *
 * Parameters:
 *   rt        – runtime handle
 *   user_text – user message text (may be NULL)
 *   on_chunk  – streaming event callback (may be NULL)
 *   user_data – opaque pointer passed to on_chunk
 *
 * Returns 0 on success, -1 on error.
 */
int agent_loop_run(llm_runtime_t *rt,
                   const char *user_text,
                   llm_runtime_callback_t on_chunk,
                   void *user_data);

#endif /* AGENT_LOOP_H */
