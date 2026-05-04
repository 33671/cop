/*
 * history_db.h
 *
 * SQLite-backed conversation history storage.
 * Database path: ~/.agent/history.sql
 *
 * Usage:
 *   history_db_t *db;
 *   history_db_open(&db);
 *
 *   int64_t sid = history_db_new_session(db);
 *   int saved = 0;
 *
 *   // After each llm_runtime_send() completes:
 *   const cJSON *history = llm_runtime_get_history(rt);
 *   const cJSON *msgs   = cJSON_GetObjectItem(history, "messages");
 *   history_db_save_step(db, sid, &saved, msgs);
 *
 *   history_db_close(db);
 */

#ifndef HISTORY_DB_H
#define HISTORY_DB_H

#include "cjson/cJSON.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct history_db history_db_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Open (or create) the history database at ~/.agent/history.sql.
 * Creates parent directories and tables automatically.
 * Returns 0 on success, -1 on error.
 */
int history_db_open(history_db_t **db_out);

/* Close and free the database handle. */
void history_db_close(history_db_t *db);

/* ============================================================================
 * Sessions
 * ============================================================================ */

/*
 * Create a new conversation session associated with a working directory.
 * cwd can be NULL (treated as "").
 * Returns the new session id on success, -1 on error.
 */
int64_t history_db_new_session(history_db_t *db, const char *cwd);

/*
 * List sessions for the given working directory, most recent first.
 * Each entry includes {id, created_at, msg_count, last_user_msg}.
 * Returns a cJSON array. Caller must cJSON_Delete().
 * Returns NULL on error.
 */
cJSON *history_db_list_sessions(history_db_t *db, const char *cwd);

/*
 * Delete a session and all its messages.
 * Returns 0 on success, -1 on error.
 */
int history_db_delete_session(history_db_t *db, int64_t session_id);

/* ============================================================================
 * Messages
 * ============================================================================ */

/*
 * Save new messages from a cJSON messages array.
 *
 * Uses *saved_count to track how many messages have been stored so far
 * for this session — only messages at indices >= *saved_count are inserted.
 * On return, *saved_count is updated to the new total.
 *
 * The cJSON array must contain standard OpenAI-format message objects:
 *   user:        {"role":"user", "content":"..."}
 *   assistant:   {"role":"assistant", "content":"...",
 *                 "reasoning_content":"...", "tool_calls":[...]}
 *   tool:        {"role":"tool", "tool_call_id":"...", "content":"..."}
 *   system:      {"role":"system", "content":"..."}
 *
 * All operations are wrapped in a single transaction.
 * Returns 0 on success, -1 on error.
 */
int history_db_save_step(history_db_t *db, int64_t session_id,
                          int *saved_count, const cJSON *messages);

/*
 * Load all messages for a session, ordered by msg_index ascending.
 * Returns a cJSON array of message objects.
 * Caller must cJSON_Delete() the result.
 * Returns NULL on error or if session not found.
 */
cJSON *history_db_load_session(history_db_t *db, int64_t session_id);

/*
 * Get the number of saved messages for a session (for resuming).
 * Returns count on success, -1 on error.
 */
int history_db_get_saved_count(history_db_t *db, int64_t session_id);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_DB_H */
