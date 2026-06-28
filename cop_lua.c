/*
 * cop_lua.c
 *
 * Bridge implementation: shared Lua state with captured print output.
 *
 * Design:
 *   - Single global lua_State* created on first use.
 *   - Overrides print() so all output goes to a dynamically grown buffer.
 *   - Provides cop_lua_execute() for safe, protected execution.
 *   - Registers a small set of helper C functions (pwd, ls, read_file)
 *     so Lua scripts can interact with the project.
 */

#include "cop_lua.h"
#include "sqlite/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Global Lua State
 * ============================================================================ */

static lua_State *G = NULL;

/* ============================================================================
 * Captured Print Buffer (thread-local via simple global — cop is single-threaded)
 * ============================================================================ */

static char *print_buf = NULL;
static size_t print_len = 0;
static size_t print_cap = 0;

/* Ensure the print buffer can hold at least 'needed' bytes. */
static void print_buf_ensure(size_t needed) {
    if (needed > print_cap) {
        size_t new_cap = print_cap ? print_cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        char *p = (char *)realloc(print_buf, new_cap);
        if (!p) return; /* silently ignore OOM */
        print_buf = p;
        print_cap = new_cap;
    }
}

static void print_buf_append(const char *s, size_t n) {
    if (!s || n == 0) return;
    print_buf_ensure(print_len + n + 1);
    memcpy(print_buf + print_len, s, n);
    print_len += n;
    print_buf[print_len] = '\0';
}

static void print_buf_reset(void) {
    print_len = 0;
    if (print_buf) print_buf[0] = '\0';
}

/* ============================================================================
 * Custom print() function for Lua
 * ============================================================================ */

/*
 * Our replacement for Lua's built-in print().
 * Concatenates all arguments (converted to string via tostring) separated
 * by tabs, followed by a newline, and appends to the capture buffer.
 */
static int lua_print_handler(lua_State *L) {
    int n = lua_gettop(L);
    lua_getglobal(L, "tostring");

    for (int i = 1; i <= n; i++) {
        if (i > 1) print_buf_append("\t", 1);

        lua_pushvalue(L, -1);           /* tostring */
        lua_pushvalue(L, i);            /* arg */
        lua_call(L, 1, 1);             /* call tostring(arg) */
        const char *s = lua_tostring(L, -1);
        if (s) print_buf_append(s, strlen(s));
        lua_pop(L, 1);                  /* pop result */
    }
    print_buf_append("\n", 1);
    return 0;
}

/* ============================================================================
 * Helper C functions exposed to Lua
 * ============================================================================ */

/* cop.pwd() → returns current working directory as string */
static int l_cop_pwd(lua_State *L) {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) {
        lua_pushstring(L, buf);
    } else {
        lua_pushstring(L, "(error getting cwd)");
    }
    return 1;
}

/* cop.ls([path]) → returns table of filenames, or nil+error */
static int l_cop_ls(lua_State *L) {
    const char *path = luaL_optstring(L, 1, ".");
    char cmd[4096];
    int r = snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>&1", path);
    if (r < 0 || (size_t)r >= sizeof(cmd)) {
        lua_pushnil(L);
        lua_pushstring(L, "path too long");
        return 2;
    }
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        lua_pushnil(L);
        lua_pushstring(L, "popen failed");
        return 2;
    }
    lua_newtable(L);
    char line[4096];
    int idx = 1;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        lua_pushinteger(L, idx++);
        lua_pushstring(L, line);
        lua_settable(L, -3);
    }
    pclose(fp);
    return 1;
}

/* cop.read_file(path) → returns file content as string, or nil+error */
static int l_cop_read_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open file: %s", path);
        return 2;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); lua_pushnil(L); lua_pushstring(L, "ftell failed"); return 2; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); lua_pushnil(L); lua_pushstring(L, "OOM"); return 2; }
    size_t nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[nread] = '\0';
    lua_pushlstring(L, buf, nread);
    free(buf);
    return 1;
}

/* cop.write_file(path, content) → writes string content to file */
static int l_cop_write_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *content = luaL_checklstring(L, 2, &len);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open file for writing: %s", path);
        return 2;
    }
    size_t written = fwrite(content, 1, len, fp);
    fclose(fp);
    if (written != len) {
        lua_pushnil(L);
        lua_pushfstring(L, "wrote %zu/%zu bytes", written, len);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* cop.shell(cmd) → runs shell command, returns {output=..., exit_code=...} */
static int l_cop_shell(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    char full_cmd[8192];
    int r = snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1; echo EXIT:$?", cmd);
    if (r < 0 || (size_t)r >= sizeof(full_cmd)) {
        lua_pushnil(L);
        lua_pushstring(L, "command too long");
        return 2;
    }
    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        lua_pushnil(L);
        lua_pushstring(L, "popen failed");
        return 2;
    }
    /* Read output */
    size_t out_len = 0, out_cap = 4096;
    char *out = (char *)malloc(out_cap);
    if (!out) { pclose(fp); lua_pushnil(L); lua_pushstring(L, "OOM"); return 2; }
    out[0] = '\0';

    char line[4096];
    int exit_code = -1;
    while (fgets(line, sizeof(line), fp)) {
        /* Check for our exit marker */
        if (strncmp(line, "EXIT:", 5) == 0) {
            exit_code = atoi(line + 5);
            break;
        }
        size_t llen = strlen(line);
        if (out_len + llen + 1 > out_cap) {
            out_cap *= 2;
            char *p = (char *)realloc(out, out_cap);
            if (!p) { free(out); pclose(fp); lua_pushnil(L); lua_pushstring(L, "OOM"); return 2; }
            out = p;
        }
        memcpy(out + out_len, line, llen);
        out_len += llen;
        out[out_len] = '\0';
    }
    pclose(fp);

    lua_newtable(L);
    lua_pushstring(L, "output");
    lua_pushstring(L, out);
    lua_settable(L, -3);
    lua_pushstring(L, "exit_code");
    lua_pushinteger(L, exit_code);
    lua_settable(L, -3);
    free(out);
    return 1;
}

/* ============================================================================
 * SQLite3 wrappers (built-in, no dynamic loading needed)
 * ============================================================================ */

/* Metatable name for SQLite database userdata */
#define SQLITE_DB_MT "cop.sqlite_db"

/* Structure stored in userdata */
typedef struct {
    sqlite3 *db;
} sqlite_db_t;

/* __gc metamethod: close database automatically */
static int l_sqlite_db_gc(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (p && p->db) {
        sqlite3_close(p->db);
        p->db = NULL;
    }
    return 0;
}

/* __tostring metamethod */
static int l_sqlite_db_tostring(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (p && p->db) {
        const char *filename = sqlite3_db_filename(p->db, "main");
        lua_pushfstring(L, "sqlite3[%s]", filename ? filename : "(memory)");
    } else {
        lua_pushstring(L, "sqlite3[closed]");
    }
    return 1;
}

/* Push a new db userdata onto the stack */
static sqlite_db_t *push_sqlite_db(lua_State *L, sqlite3 *db) {
    sqlite_db_t *p = (sqlite_db_t *)lua_newuserdata(L, sizeof(sqlite_db_t));
    p->db = db;

    /* Set metatable for garbage collection */
    if (luaL_newmetatable(L, SQLITE_DB_MT)) {
        /* First time: set up the metatable */
        lua_pushcfunction(L, l_sqlite_db_gc);
        lua_setfield(L, -2, "__gc");

        lua_pushcfunction(L, l_sqlite_db_tostring);
        lua_setfield(L, -2, "__tostring");

        /* Make the metatable its own __index (for db:method support if needed) */
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
    return p;
}

/* cop.sqlite_open(filename) → db_handle or nil+error */
static int l_cop_sqlite_open(lua_State *L) {
    const char *path = luaL_optstring(L, 1, ":memory:");
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        if (db) {
            lua_pushnil(L);
            lua_pushfstring(L, "%s", sqlite3_errmsg(db));
            sqlite3_close(db);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, "out of memory");
        }
        return 2;
    }
    push_sqlite_db(L, db);
    return 1;
}

/* cop.sqlite_close(db) → true or nil+error */
static int l_cop_sqlite_close(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (!p->db) {
        lua_pushnil(L);
        lua_pushstring(L, "database already closed");
        return 2;
    }
    int rc = sqlite3_close(p->db);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s", sqlite3_errmsg(p->db));
        return 2;
    }
    p->db = NULL;
    lua_pushboolean(L, 1);
    return 1;
}

/* Column-type helper: push a single column value onto Lua stack */
static void push_column_value(lua_State *L, sqlite3_stmt *stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_NULL:
            lua_pushnil(L);
            break;
        case SQLITE_INTEGER:
            lua_pushinteger(L, sqlite3_column_int64(stmt, col));
            break;
        case SQLITE_FLOAT:
            lua_pushnumber(L, sqlite3_column_double(stmt, col));
            break;
        default:
            lua_pushstring(L, (const char *)sqlite3_column_text(stmt, col));
            break;
    }
}

/* cop.sqlite_exec(db, sql) → table with info, or nil+error */
static int l_cop_sqlite_exec(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (!p->db) {
        lua_pushnil(L);
        lua_pushstring(L, "database is closed");
        return 2;
    }
    const char *sql = luaL_checkstring(L, 2);

    char *errmsg = NULL;
    int rc = sqlite3_exec(p->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        return 2;
    }

    lua_newtable(L);
    lua_pushstring(L, "rows_affected");
    lua_pushinteger(L, sqlite3_changes(p->db));
    lua_settable(L, -3);
    lua_pushstring(L, "last_insert_id");
    lua_pushinteger(L, sqlite3_last_insert_rowid(p->db));
    lua_settable(L, -3);
    return 1;
}

/* cop.sqlite_get(db, sql) → array of row-tables, or nil+error */
static int l_cop_sqlite_get(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (!p->db) {
        lua_pushnil(L);
        lua_pushstring(L, "database is closed");
        return 2;
    }
    const char *sql = luaL_checkstring(L, 2);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(p->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s", sqlite3_errmsg(p->db));
        return 2;
    }

    lua_newtable(L); /* result array */
    int row_idx = 1;
    int ncols = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (ncols == 0) ncols = sqlite3_column_count(stmt);

        lua_newtable(L); /* one row */

        for (int i = 0; i < ncols; i++) {
            /* Numeric index (1-based) */
            lua_pushinteger(L, i + 1);
            push_column_value(L, stmt, i);
            lua_settable(L, -3);

            /* Named index (column name) */
            const char *name = sqlite3_column_name(stmt, i);
            if (name) {
                lua_pushstring(L, name);
                /* Need to push value again */
                push_column_value(L, stmt, i);
                lua_settable(L, -3);
            }
        }

        lua_rawseti(L, -2, row_idx++);
    }

    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        lua_pushnil(L);
        lua_pushfstring(L, "%s", sqlite3_errmsg(p->db));
        return 2;
    }

    sqlite3_finalize(stmt);
    return 1; /* the result table */
}

/* cop.sqlite_get_one(db, sql) → one row or nil, or nil+error */
static int l_cop_sqlite_get_one(lua_State *L) {
    sqlite_db_t *p = (sqlite_db_t *)luaL_checkudata(L, 1, SQLITE_DB_MT);
    if (!p->db) {
        lua_pushnil(L);
        lua_pushstring(L, "database is closed");
        return 2;
    }
    const char *sql = luaL_checkstring(L, 2);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(p->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s", sqlite3_errmsg(p->db));
        return 2;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        lua_newtable(L);
        int ncols = sqlite3_column_count(stmt);
        for (int i = 0; i < ncols; i++) {
            /* Numeric index */
            lua_pushinteger(L, i + 1);
            push_column_value(L, stmt, i);
            lua_settable(L, -3);

            /* Named index */
            const char *name = sqlite3_column_name(stmt, i);
            if (name) {
                lua_pushstring(L, name);
                push_column_value(L, stmt, i);
                lua_settable(L, -3);
            }
        }
        sqlite3_finalize(stmt);
        return 1;
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s", sqlite3_errmsg(p->db));
        return 2;
    }

    lua_pushnil(L); /* no rows returned */
    return 1;
}

/* ============================================================================
 * Register helpers into a "cop" global table
 * ============================================================================ */

static void register_cop_helpers(lua_State *L) {
    lua_newtable(L);   /* cop = {} */

    lua_pushcfunction(L, l_cop_pwd);
    lua_setfield(L, -2, "pwd");

    lua_pushcfunction(L, l_cop_ls);
    lua_setfield(L, -2, "ls");

    lua_pushcfunction(L, l_cop_read_file);
    lua_setfield(L, -2, "read_file");

    lua_pushcfunction(L, l_cop_write_file);
    lua_setfield(L, -2, "write_file");

    lua_pushcfunction(L, l_cop_shell);
    lua_setfield(L, -2, "shell");

    /* SQLite functions */
    lua_pushcfunction(L, l_cop_sqlite_open);
    lua_setfield(L, -2, "sqlite_open");

    lua_pushcfunction(L, l_cop_sqlite_close);
    lua_setfield(L, -2, "sqlite_close");

    lua_pushcfunction(L, l_cop_sqlite_exec);
    lua_setfield(L, -2, "sqlite_exec");

    lua_pushcfunction(L, l_cop_sqlite_get);
    lua_setfield(L, -2, "sqlite_get");

    lua_pushcfunction(L, l_cop_sqlite_get_one);
    lua_setfield(L, -2, "sqlite_get_one");

    lua_setglobal(L, "cop");
}

/* ============================================================================
 * Public API
 * ============================================================================ */

lua_State *cop_lua_global_state(void) {
    if (G) return G;

    G = luaL_newstate();
    if (!G) return NULL;

    luaL_openlibs(G);

    /* Override print with our capture version */
    lua_pushcfunction(G, lua_print_handler);
    lua_setglobal(G, "print");

    /* Register cop.* helper functions */
    register_cop_helpers(G);

    return G;
}

int cop_lua_execute(const char *code, char **output) {
    if (!output) return -1;
    *output = NULL;

    lua_State *L = cop_lua_global_state();
    if (!L) {
        *output = strdup("Fatal: failed to initialize Lua state");
        return -1;
    }

    /* Reset the print buffer before execution */
    print_buf_reset();

    /* Load and execute the chunk in protected mode */
    int status = luaL_loadstring(L, code);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        *output = strdup(err ? err : "(unknown load error)");
        lua_pop(L, 1);
        return -1;
    }

    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        *output = strdup(err ? err : "(unknown exec error)");
        lua_pop(L, 1);
        return -1;
    }

    /* Return captured print output, or empty string if nothing was printed */
    if (print_len > 0) {
        *output = strdup(print_buf);
    } else {
        *output = strdup("");
    }
    return 0;
}

void cop_lua_cleanup(void) {
    if (G) {
        lua_close(G);
        G = NULL;
    }
    free(print_buf);
    print_buf = NULL;
    print_len = 0;
    print_cap = 0;
}
