/*
 * tool_functions.h
 *
 * Example tool function implementations and schema helpers for the LLM runtime.
 * These demonstrate how to write tool handlers with cancellation support,
 * async subprocess execution, file I/O, etc.
 */

#ifndef TOOL_FUNCTIONS_H
#define TOOL_FUNCTIONS_H

#include "cjson/cJSON.h"
#include "llm_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Tool Handlers
 * ============================================================================ */

/*
 * Sleep tool: sleeps for a given number of seconds with cancellation support.
 * Sleeps in small increments (0.1s steps) to detect cancellation promptly.
 *
 * Args: {"secs": <number>}
 * Returns: {"type": "text", "text": "done, slept for X seconds"}  or
 *          {"type": "text", "text": "cancelled"} on cancellation
 */
cJSON *tool_sleep(llm_runtime_t *rt, const cJSON *args);

/*
 * Shell tool: runs a shell command asynchronously via llm_runtime_popen().
 * Captures merged stdout+stderr, returns output and exit code.
 * Truncates output longer than 4000 characters.
 * Requires user approval before execution.
 *
 * Args: {"cmd": "<shell command>"}
 * Returns: {"type": "text", "text": "Output:\n<output>\nExit_code:<code>"}
 */
cJSON *tool_shell(llm_runtime_t *rt, const cJSON *args);

/*
 * Read tool: reads selected lines from a file.
 * Supports line offset and limit for partial reads.
 * No user approval required (read-only).
 *
 * Args: {"path": "<filepath>", "offset": <int>, "limit": <int>}
 *   offset – starting line number (1-indexed), default 1
 *   limit  – maximum number of lines to return, default 1000, max 1000
 * Returns: {"type": "text", "text": "<file contents>"}
 */
cJSON *tool_read(llm_runtime_t *rt, const cJSON *args);

/*
 * Write tool: writes or appends content to a file.
 * Creates parent directories if they don't exist.
 * Requires user approval before writing — shows a preview of the
 * path and content.
 *
 * Args: {"path": "<filepath>", "content": "<content>", "mode": "<str>"}
 *   mode – "overwrite", "append", or NULL/absent.
 *          If NULL and file exists, returns an error asking the
 *          caller to specify mode explicitly.
 * Returns: {"type": "text", "text": "Successfully wrote to <abs_path>"}
 */
cJSON *tool_write(llm_runtime_t *rt, const cJSON *args);

/*
 * Edit tool: replaces substring(s) in a file.
 * Checks file existence, shows a preview diff, and requires
 * user approval before applying.
 *
 * Args: {"path": "<filepath>", "old": "<substring to replace>",
 *        "new": "<replacement>", "replace_all": <bool>}
 *   old         – substring to find and replace
 *   new         – replacement substring
 *   replace_all – if true replace all occurrences, otherwise only first
 * Returns: {"type": "text", "text": "Successfully replaced in <abs_path>"}
 */
cJSON *tool_edit(llm_runtime_t *rt, const cJSON *args);

/* ============================================================================
 * Tool Schema Helpers
 * ============================================================================ */

void tool_functions_add_sleep_schema(cJSON *schemas);
void tool_functions_add_shell_schema(cJSON *schemas);
void tool_functions_add_read_schema(cJSON *schemas);
void tool_functions_add_write_schema(cJSON *schemas);
void tool_functions_add_edit_schema(cJSON *schemas);

/*
 * Create a complete cJSON array with all tool schemas.
 * The caller owns the returned array and must free it with cJSON_Delete().
 *
 * Includes: sleep, shell, read, write, edit
 *
 * Returns: a new cJSON array, or NULL on allocation failure.
 */
cJSON *tool_functions_create_schema(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_FUNCTIONS_H */
