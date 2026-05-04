/*
 * history_db.c
 *
 * SQLite-backed conversation history storage implementation.
 */

#include "history_db.h"
#include "sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ============================================================================
 * Internal Structure
 * ============================================================================ */
struct history_db {
    sqlite3 *conn;
};

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Expand ~ in path. Caller must free the result. */
static char *expand_path(const char *path) {
    if (!path) return NULL;

    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";

        size_t len = strlen(home) + strlen(path); /* path includes ~ */
        char *result = malloc(len);
        if (!result) return NULL;
        snprintf(result, len, "%s%s", home, path + 1);
        return result;
    }

    return strdup(path);
}

/* Create parent directories for a file path. */
static int mkdir_p(const char *filepath) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);

    /* Find the last '/' to get the directory part */
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return 0;  /* no directory component */
    *last_slash = '\0';

    /* Walk through and create each directory level */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return 0;
}

/* Execute a single SQL statement (no bindings, no result). */
static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[history_db] SQL error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

int history_db_open(history_db_t **db_out) {
    if (!db_out) return -1;

    *db_out = NULL;

    history_db_t *db = calloc(1, sizeof(history_db_t));
    if (!db) return -1;

    char *db_path = expand_path("~/.agent/history.sql");
    if (!db_path) {
        free(db);
        return -1;
    }

    /* Ensure parent directory exists */
    mkdir_p(db_path);

    int rc = sqlite3_open(db_path, &db->conn);
    free(db_path);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[history_db] cannot open database: %s\n",
                sqlite3_errmsg(db->conn));
        free(db);
        return -1;
    }

    /* Enable WAL mode for better concurrency */
    exec_sql(db->conn, "PRAGMA journal_mode=WAL");
    exec_sql(db->conn, "PRAGMA foreign_keys=ON");

    /* Create schema */
    const char *schema =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cwd        TEXT,"
        "  name       TEXT,"
        "  created_at TEXT DEFAULT (datetime('now'))"
        ");"

        "CREATE TABLE IF NOT EXISTS messages ("
        "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id        INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "  msg_index         INTEGER NOT NULL,"
        "  role              TEXT NOT NULL,"
        "  content           TEXT,"
        "  reasoning_content TEXT,"
        "  tool_call_id      TEXT,"
        "  tool_calls        TEXT,"
        "  created_at        TEXT DEFAULT (datetime('now'))"
        ");"

        "CREATE INDEX IF NOT EXISTS idx_messages_session "
        "  ON messages(session_id, msg_index);"

        "CREATE INDEX IF NOT EXISTS idx_sessions_cwd "
        "  ON sessions(cwd, id DESC);";

    if (exec_sql(db->conn, schema) != 0) {
        sqlite3_close(db->conn);
        free(db);
        return -1;
    }

    /* Migration: add cwd column to pre-existing databases (ignore duplicate) */
    {
        char *err = NULL;
        sqlite3_exec(db->conn, "ALTER TABLE sessions ADD COLUMN cwd TEXT",
                     NULL, NULL, &err);
        if (err) sqlite3_free(err);  /* silently ignore "duplicate column" */
    }

    *db_out = db;
    return 0;
}

void history_db_close(history_db_t *db) {
    if (!db) return;
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = NULL;
    }
    free(db);
}

/* ============================================================================
 * Sessions
 * ============================================================================ */

int64_t history_db_new_session(history_db_t *db, const char *cwd) {
    if (!db || !db->conn) return -1;

    const char *sql =
        "INSERT INTO sessions (cwd, name) VALUES (?,"
        "  'Chat ' || datetime('now','localtime')"
        ")";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] new_session prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, cwd ? cwd : "", -1, SQLITE_STATIC);

    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(db->conn);
    }

    sqlite3_finalize(stmt);
    return id;
}

cJSON *history_db_list_sessions(history_db_t *db, const char *cwd) {
    if (!db || !db->conn) return NULL;

    /*
     * JOIN with a subquery to get the last user message content per session.
     * Also include message count and created_at.
     */
    const char *sql =
        "SELECT s.id, s.created_at,"
        "  (SELECT COUNT(*) FROM messages m2 WHERE m2.session_id = s.id) AS msg_count,"
        "  (SELECT m3.content FROM messages m3"
        "    WHERE m3.session_id = s.id AND m3.role = 'user'"
        "    ORDER BY m3.msg_index DESC LIMIT 1) AS last_user_msg "
        "FROM sessions s "
        "WHERE s.cwd = ? "
        "ORDER BY s.id DESC";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] list_sessions prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, cwd ? cwd : "", -1, SQLITE_STATIC);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *session = cJSON_CreateObject();
        cJSON_AddNumberToObject(session, "id",
                                sqlite3_column_int64(stmt, 0));
        cJSON_AddStringToObject(session, "created_at",
            (const char *)sqlite3_column_text(stmt, 1));
        cJSON_AddNumberToObject(session, "msg_count",
                                sqlite3_column_int(stmt, 2));

        const char *last_msg = (const char *)sqlite3_column_text(stmt, 3);
        if (last_msg) {
            /* Truncate long messages for display */
            size_t mlen = strlen(last_msg);
            char preview[81];
            if (mlen > 80) {
                memcpy(preview, last_msg, 77);
                memcpy(preview + 77, "...", 4);
            } else {
                memcpy(preview, last_msg, mlen + 1);
            }
            cJSON_AddStringToObject(session, "last_user_msg", preview);
        } else {
            cJSON_AddNullToObject(session, "last_user_msg");
        }

        cJSON_AddItemToArray(arr, session);
    }

    sqlite3_finalize(stmt);
    return arr;
}

int history_db_delete_session(history_db_t *db, int64_t session_id) {
    if (!db || !db->conn) return -1;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "DELETE FROM sessions WHERE id = ?";

    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] delete_session prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ============================================================================
 * Messages — Save
 * ============================================================================ */

/*
 * Extract a string field from a cJSON object, returns "" if missing.
 * The returned pointer is into cJSON's memory — do not free.
 */
static const char *json_get_string(const cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

int history_db_save_step(history_db_t *db, int64_t session_id,
                          int *saved_count, const cJSON *messages) {
    if (!db || !db->conn || !saved_count || !messages) return -1;
    if (!cJSON_IsArray(messages)) return -1;

    int total = cJSON_GetArraySize(messages);
    if (total <= *saved_count) return 0;  /* nothing new */

    const char *sql =
        "INSERT INTO messages (session_id, msg_index, role, content, "
        "reasoning_content, tool_call_id, tool_calls) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] save_step prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return -1;
    }

    /* Wrap in transaction */
    exec_sql(db->conn, "BEGIN TRANSACTION");

    int ok = 1;
    for (int i = *saved_count; i < total; i++) {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        if (!msg) continue;

        const char *role      = json_get_string(msg, "role");
        const char *content   = json_get_string(msg, "content");
        const char *reasoning = json_get_string(msg, "reasoning_content");
        const char *tc_id     = json_get_string(msg, "tool_call_id");

        /* Serialize tool_calls array to JSON string if present */
        char *tool_calls_str = NULL;
        cJSON *tc = cJSON_GetObjectItem(msg, "tool_calls");
        if (tc && (cJSON_IsArray(tc) || cJSON_IsObject(tc))) {
            tool_calls_str = cJSON_PrintUnformatted(tc);
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_int(stmt,  2, i);                /* msg_index */
        sqlite3_bind_text(stmt, 3, role,      -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, content,   -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, reasoning, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, tc_id,     -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, tool_calls_str, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "[history_db] save_step insert: %s\n",
                    sqlite3_errmsg(db->conn));
            ok = 0;
            free(tool_calls_str);
            break;
        }

        free(tool_calls_str);
    }

    sqlite3_finalize(stmt);

    if (ok) {
        exec_sql(db->conn, "COMMIT");
        *saved_count = total;
        return 0;
    } else {
        exec_sql(db->conn, "ROLLBACK");
        return -1;
    }
}

/* ============================================================================
 * Messages — Load
 * ============================================================================ */

cJSON *history_db_load_session(history_db_t *db, int64_t session_id) {
    if (!db || !db->conn) return NULL;

    const char *sql =
        "SELECT role, content, reasoning_content, tool_call_id, tool_calls "
        "FROM messages WHERE session_id = ? ORDER BY msg_index ASC";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] load_session prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return NULL;
    }

    sqlite3_bind_int64(stmt, 1, session_id);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *msg = cJSON_CreateObject();

        const char *role      = (const char *)sqlite3_column_text(stmt, 0);
        const char *content   = (const char *)sqlite3_column_text(stmt, 1);
        const char *reasoning = (const char *)sqlite3_column_text(stmt, 2);
        const char *tc_id     = (const char *)sqlite3_column_text(stmt, 3);
        const char *tc_json   = (const char *)sqlite3_column_text(stmt, 4);

        cJSON_AddStringToObject(msg, "role", role ? role : "");

        if (content) {
            cJSON_AddStringToObject(msg, "content", content);
        }

        if (reasoning && reasoning[0]) {
            cJSON_AddStringToObject(msg, "reasoning_content", reasoning);
        }

        if (tc_id && tc_id[0]) {
            cJSON_AddStringToObject(msg, "tool_call_id", tc_id);
        }

        if (tc_json && tc_json[0]) {
            cJSON *tc_parsed = cJSON_Parse(tc_json);
            if (tc_parsed) {
                cJSON_AddItemToObject(msg, "tool_calls", tc_parsed);
            }
        }

        cJSON_AddItemToArray(arr, msg);
    }

    sqlite3_finalize(stmt);
    return arr;
}

/* ============================================================================
 * Messages — Count
 * ============================================================================ */

int history_db_get_saved_count(history_db_t *db, int64_t session_id) {
    if (!db || !db->conn) return -1;

    const char *sql = "SELECT COUNT(*) FROM messages WHERE session_id = ?";
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[history_db] get_saved_count prepare: %s\n",
                sqlite3_errmsg(db->conn));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, session_id);

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}
