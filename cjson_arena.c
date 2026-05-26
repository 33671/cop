/*
 * cjson_arena.c
 *
 * Hooks cJSON's global allocator to an arena so all cJSON nodes
 * (create / parse / delete) bypass malloc/free entirely.
 *
 * cjson_arena_init() must be called once before any cJSON usage.
 * The arena is never freed during the session — it grows monotonically
 * and is reclaimed by the OS on exit.  If per-step cleanup is needed,
 * snapshots and rewind can be added later.
 */

#include "cJSON.h"
#include "arena.h"

static Arena cjson_arena = {0};

static void *cjson_arena_malloc(size_t size) {
    return arena_alloc(&cjson_arena, size);
}

static void cjson_arena_free(void *ptr) {
    (void)ptr;  /* arena never frees individual objects */
}

void cjson_arena_init(void) {
    cJSON_Hooks hooks;
    hooks.malloc_fn = cjson_arena_malloc;
    hooks.free_fn   = cjson_arena_free;
    cJSON_InitHooks(&hooks);
}
