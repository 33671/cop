/*
 * cjson_arena.h
 *
 * Hooks cJSON's global allocator to an arena so all cJSON nodes
 * (create / parse / delete) bypass malloc/free entirely.
 *
 * cjson_arena_init() must be called once before any cJSON usage.
 * cjson_arena_trim() can be called at step or turn boundaries to
 * release unused arena regions (keeps current region data intact).
 */

#ifndef CJSON_ARENA_H
#define CJSON_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise cJSON to use an internal arena.  Must be called once
 * before any cJSON_CreateXXX / cJSON_Parse / cJSON_Delete. */
void cjson_arena_init(void);

/*
 * Trim unused trailing regions from the internal cJSON arena.
 *
 * Only regions that are completely unused (inaccessible via the
 * current end region) are freed.  Active cJSON objects are safe.
 *
 * Call this at step / turn boundaries to prevent monotonic memory
 * growth from temporary cJSON nodes that cannot be individually freed.
 */
void cjson_arena_trim(void);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_ARENA_H */
