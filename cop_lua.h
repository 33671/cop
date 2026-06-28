/*
 * cop_lua.h
 *
 * Bridge between the cop C project and the embedded Lua interpreter.
 * Provides a single shared Lua state and safe execution primitives.
 */

#ifndef COP_LUA_H
#define COP_LUA_H

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a global Lua state with standard libraries loaded.
 * Returns NULL on failure.
 * Safe to call multiple times; subsequent calls are no-ops returning
 * the existing state.
 */
lua_State *cop_lua_global_state(void);

/*
 * Execute a chunk of Lua code in the global state.
 *
 *   code   – Lua source code string (UTF-8)
 *   output – out-parameter: pointer that receives a malloc'd string
 *            with captured print() output. Caller must free() it.
 *            Set to NULL if there is no output.
 *
 * Returns 0 on success, -1 on error (error message is set in *output).
 */
int cop_lua_execute(const char *code, char **output);

/*
 * Destroy the global Lua state (if any). Safe to call at shutdown.
 */
void cop_lua_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* COP_LUA_H */
