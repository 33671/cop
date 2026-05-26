#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "sds.h"

int main(void)
{
    Arena a = {0};

    printf("=== Arena SDS tests ===\n");

    /* 1. Basic create / dup / free */
    {
        sds s1 = sdsnew(&a, "hello");
        assert(sdslen(s1) == 5);
        assert(strcmp(s1, "hello") == 0);
        printf("[PASS] sdsnew + sdslen\n");

        sds s2 = sdsdup(s1);
        assert(sdslen(s2) == 5);
        assert(strcmp(s2, "hello") == 0);
        printf("[PASS] sdsdup (same arena)\n");

        /* cross-arena dup */
        {
            Arena ar2 = {0};
            sds s3 = sdsdupTo(&ar2, s1);
            assert(sdslen(s3) == 5);
            assert(strcmp(s3, "hello") == 0);
            assert(s3 != s1);  /* different pointer, different arena */
            arena_free(&ar2);
            printf("[PASS] sdsdupTo (cross-arena)\n");
        }
    }

    /* 2. sdsempty + sdscat */
    {
        sds s = sdsempty(&a);
        assert(sdslen(s) == 0);
        printf("[PASS] sdsempty\n");

        s = sdscat(s, "foo");
        s = sdscat(s, "bar");
        assert(sdslen(s) == 6);
        assert(strcmp(s, "foobar") == 0);
        printf("[PASS] sdscat\n");
    }

    /* 3. sdsnewlen (binary safe) */
    {
        sds s = sdsnewlen(&a, "ab\0cd", 5);
        assert(sdslen(s) == 5);
        assert(memcmp(s, "ab\0cd", 5) == 0);
        printf("[PASS] sdsnewlen binary-safe\n");
    }

    /* 4. sdscatsds */
    {
        sds a1 = sdsnew(&a, "Hello, ");
        sds a2 = sdsnew(&a, "World!");
        sds joined = sdscatsds(a1, a2);
        assert(sdslen(joined) == 13);
        assert(strcmp(joined, "Hello, World!") == 0);
        printf("[PASS] sdscatsds\n");
    }

    /* 5. sdscpy / sdscpylen */
    {
        sds s = sdsnewlen(&a, "xxxxxxxxxx", 10);
        s = sdscpy(s, "abc");
        assert(sdslen(s) == 3);
        assert(strcmp(s, "abc") == 0);
        printf("[PASS] sdscpy (shorter)\n");

        s = sdscpy(s, "a very long string that needs realloc");
        assert(sdslen(s) == 37);
        assert(strcmp(s, "a very long string that needs realloc") == 0);
        printf("[PASS] sdscpy (longer, cross-type realloc)\n");
    }

    /* 6. Multi-arena: two arenas simultaneously */
    {
        Arena arena1 = {0}, arena2 = {0};

        sds s1 = sdsnew(&arena1, "from arena 1");
        sds s2 = sdsnew(&arena2, "from arena 2");

        assert(strcmp(s1, "from arena 1") == 0);
        assert(strcmp(s2, "from arena 2") == 0);

        /* Modify s2 - it should use arena2 */
        s2 = sdscat(s2, " modified");
        assert(strcmp(s2, "from arena 2 modified") == 0);

        arena_free(&arena1);
        arena_free(&arena2);
        printf("[PASS] Multi-arena\n");
    }

    /* 7. sdsMakeRoomFor + sdsIncrLen (low-level API) */
    {
        sds s = sdsnewlen(&a, "abc", 3);
        size_t oldlen = sdslen(s);
        s = sdsMakeRoomFor(s, 10);
        assert(sdslen(s) == oldlen);  /* length unchanged */
        assert(sdsavail(s) >= 10);    /* but space available */

        /* Manually write and commit */
        memcpy(s + oldlen, "defghij", 7);
        sdsIncrLen(s, 7);
        assert(sdslen(s) == 10);
        s[10] = '\0';
        assert(strcmp(s, "abcdefghij") == 0);
        printf("[PASS] sdsMakeRoomFor + sdsIncrLen\n");
    }

    /* 8. sdstrim */
    {
        sds s = sdsnew(&a, "   hello world   ");
        s = sdstrim(s, " ");
        assert(sdslen(s) == 11);
        assert(strcmp(s, "hello world") == 0);
        printf("[PASS] sdstrim\n");
    }

    /* 9. sdsrange */
    {
        sds s = sdsnew(&a, "Hello World");
        sdsrange(s, 0, 4);
        assert(strcmp(s, "Hello") == 0);
        printf("[PASS] sdsrange\n");
    }

    /* 10. sdscmp */
    {
        sds s1 = sdsnew(&a, "abc");
        sds s2 = sdsnew(&a, "abd");
        assert(sdscmp(s1, s2) < 0);
        assert(sdscmp(s2, s1) > 0);

        sds s3 = sdsnew(&a, "abc");
        assert(sdscmp(s1, s3) == 0);
        printf("[PASS] sdscmp\n");
    }

    /* 11. sdssplitlen */
    {
        int count;
        sds *tokens = sdssplitlen(&a, "a,b,c,d", 7, ",", 1, &count);
        assert(count == 4);
        assert(strcmp(tokens[0], "a") == 0);
        assert(strcmp(tokens[1], "b") == 0);
        assert(strcmp(tokens[2], "c") == 0);
        assert(strcmp(tokens[3], "d") == 0);
        sdsfreesplitres(tokens, count);
        printf("[PASS] sdssplitlen\n");
    }

    /* 12. sdsjoin */
    {
        char *words[] = {"this", "is", "a", "test"};
        sds s = sdsjoin(&a, words, 4, " ");
        assert(strcmp(s, "this is a test") == 0);
        printf("[PASS] sdsjoin\n");
    }

    /* 13. sdsfromlonglong */
    {
        sds s = sdsfromlonglong(&a, -12345);
        assert(strcmp(s, "-12345") == 0);
        printf("[PASS] sdsfromlonglong\n");
    }

    /* 14. sdscatfmt */
    {
        sds s = sdsnew(&a, ">> ");
        s = sdscatfmt(s, "int=%i, str=%s", 42, "hello");
        assert(strcmp(s, ">> int=42, str=hello") == 0);
        printf("[PASS] sdscatfmt\n");
    }

    /* 15. sdscatprintf */
    {
        sds s = sdsempty(&a);
        s = sdscatprintf(s, "%d + %d = %d", 1, 2, 3);
        assert(strcmp(s, "1 + 2 = 3") == 0);
        printf("[PASS] sdscatprintf\n");
    }

    /* 16. sdsgrowzero */
    {
        sds s = sdsnew(&a, "hi");
        s = sdsgrowzero(s, 10);
        assert(sdslen(s) == 10);
        // bytes 2-9 should be zero
        for (size_t i = 2; i < 10; i++)
            assert(s[i] == '\0');
        printf("[PASS] sdsgrowzero\n");
    }

    /* 17. arena_reset reuses memory */
    {
        arena_reset(&a);

        /* Same arena, fresh allocation */
        sds s2 = sdsnew(&a, "after reset");
        assert(strcmp(s2, "after reset") == 0);
        /* The old pointer is invalid after reset, but we don't use it */
        printf("[PASS] arena_reset + reuse\n");
    }

    /* 18. sdstolower / sdstoupper */
    {
        sds s = sdsnew(&a, "Hello World");
        sdstolower(s);
        assert(strcmp(s, "hello world") == 0);
        sdstoupper(s);
        assert(strcmp(s, "HELLO WORLD") == 0);
        printf("[PASS] sdstolower / sdstoupper\n");
    }

    /* 19. arena_realloc: last-allocation in-place extension */
    {
        Arena ar = {0};

        /* Allocate a block; being the only allocation it's also the last */
        void *p1 = arena_alloc(&ar, 64);
        assert(p1 != NULL);
        size_t count_after_p1 = ar.end->count;

        /* Realloc p1 (the last allocation) to a larger size */
        void *p2 = arena_realloc(&ar, p1, 64, 128);
        /* Should return the same pointer — no copy needed */
        assert(p1 == p2);
        /* Count should increase by only the delta, not old+new */
        size_t old_words = (64  + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);
        size_t new_words = (128 + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);
        assert(ar.end->count == count_after_p1 - old_words + new_words);
        printf("[PASS] arena_realloc last-alloc extends in-place\n");

        /* Now allocate a second block so p2 is no longer the last */
        void *p3 = arena_alloc(&ar, 32);
        assert(p3 != NULL);

        /* Realloc p2 (now NOT the last) — must alloc+copy */
        void *p4 = arena_realloc(&ar, p2, 128, 256);
        assert(p4 != p2);   /* different pointer */
        assert(p4 != NULL);
        /* Verify old data was copied */
        assert(memcmp(p4, p1, 64) == 0);
        printf("[PASS] arena_realloc non-last alloc+copy\n");

        /* Realloc with same size — should be no-op */
        void *p5 = arena_realloc(&ar, p4, 256, 256);
        assert(p5 == p4);
        printf("[PASS] arena_realloc same-size no-op\n");

        /* Realloc with smaller size — should be no-op */
        void *p6 = arena_realloc(&ar, p4, 256, 100);
        assert(p6 == p4);
        printf("[PASS] arena_realloc shrink no-op\n");

        arena_free(&ar);
    }

    /* 19b. arena_realloc: last-alloc but exceeds region capacity → alloc+copy */
    {
        Arena ar = {0};
        unsigned char pattern = 0xAB;

        /* Default region capacity is 8192 words (64KB on 64-bit).
         * 60000 bytes → 7500 words.  Realloc to 70000 bytes → 8750
         * words, which exceeds 8192, forcing alloc+copy. */
        size_t old_sz = 60000;
        void *p1 = arena_alloc(&ar, old_sz);
        assert(p1 != NULL);
        memset(p1, pattern, old_sz);

        void *p2 = arena_realloc(&ar, p1, old_sz, 70000);
        assert(p2 != NULL);
        assert(p2 != p1);  /* different pointer: new region allocated */

        /* Verify old data was copied correctly */
        unsigned char *pc = (unsigned char *)p2;
        for (size_t i = 0; i < old_sz; i++)
            assert(pc[i] == pattern);

        arena_free(&ar);
        printf("[PASS] arena_realloc last-alloc exceeds capacity → alloc+copy\n");
    }

    /* ── Tests for _arena cross-type write fix ─────────────────── */
    /* These verify that sdsMakeRoomFor() and sdsRemoveFreeSpace()
     * write the _arena pointer at the correct offset for the
     * *target* header type, not always at sdshdr8's offset.
     *
     * The bug was: ((struct sdshdr8 *)newsh)->_arena = (void*)a
     * which writes at offset 2. But different header types have
     * _arena at different offsets:
     *   sdshdr5: 0   sdshdr8: 2   sdshdr16: 4   sdshdr32: 8
     *
     * Each sub-test does a cross-type operation then immediately
     * calls sdscat() which internally calls sdsGetArena() — if
     * _arena was written at the wrong offset, the subsequent
     * arena access will read garbage and crash or corrupt. */
    {
        Arena ar = {0};

        /* 20. sdsMakeRoomFor: cross-type 5→16 */
        {
            sds s = sdsnewlen(&ar, "ab", 2);       /* type 5 (2 < 32) */
            s = sdsMakeRoomFor(s, 300);            /* newlen=604 → type 16 */
            /* Verify arena pointer is intact by appending */
            s = sdscat(s, "cd");
            assert(sdslen(s) == 4);
            assert(strcmp(s, "abcd") == 0);
            printf("[PASS] sdsMakeRoomFor cross-type 5→16 + arena verify\n");
        }

        /* 21. sdsMakeRoomFor: cross-type 8→16 */
        {
            char init[120];
            memset(init, 'x', 119);
            init[119] = '\0';
            sds s = sdsnewlen(&ar, init, 119);     /* type 8 (119 < 256, >= 32) */
            s = sdsMakeRoomFor(s, 200);            /* newlen=638 → type 16 */
            s = sdscat(s, "YY");
            assert(sdslen(s) == 121);
            assert(memcmp(s + 119, "YY", 2) == 0);
            printf("[PASS] sdsMakeRoomFor cross-type 8→16 + arena verify\n");
        }

        /* 22. sdsMakeRoomFor: cross-type 5→32 */
        {
            sds s = sdsnewlen(&ar, "xy", 2);       /* type 5 */
            s = sdsMakeRoomFor(s, 40000);          /* newlen=80004 → type 32 */
            /* Verify arena pointer is intact */
            s = sdscat(s, "z");
            assert(sdslen(s) == 3);
            assert(strcmp(s, "xyz") == 0);
            printf("[PASS] sdsMakeRoomFor cross-type 5→32 + arena verify\n");
        }

        /* 23. sdsRemoveFreeSpace: cross-type 16→5 */
        {
            sds s = sdsnewlen(&ar, "hi", 2);       /* type 5 */
            s = sdsMakeRoomFor(s, 300);            /* → type 16, alloc=604 */
            s = sdsRemoveFreeSpace(s);             /* len=2 → type 5, cross! */
            /* Verify arena pointer by appending (sdscat → sdsGetArena) */
            s = sdscat(s, "!!");
            assert(sdslen(s) == 4);
            assert(strcmp(s, "hi!!") == 0);
            printf("[PASS] sdsRemoveFreeSpace cross-type 16→5 + arena verify\n");
        }

        /* 24. sdsRemoveFreeSpace: cross-type 16→8 */
        {
            /* Create a 50-byte content string so shrink targets type 8 */
            char content[64];
            memset(content, 'A', 50);
            content[50] = '\0';
            sds s = sdsnewlen(&ar, content, 50);   /* type 8 (50 >= 32) */
            s = sdsMakeRoomFor(s, 400);            /* → type 16, alloc big */
            s = sdsRemoveFreeSpace(s);             /* len=50 → type 8, cross! */
            /* Verify arena pointer */
            s = sdscat(s, "B");
            assert(sdslen(s) == 51);
            assert(s[50] == 'B');
            printf("[PASS] sdsRemoveFreeSpace cross-type 16→8 + arena verify\n");
        }

        /* 25. sdsRemoveFreeSpace: cross-type 32→5 (covers SDS_TYPE_5 case) */
        {
            sds s = sdsnewlen(&ar, "ok", 2);       /* type 5 */
            s = sdsMakeRoomFor(s, 50000);          /* → type 32 */
            s = sdsRemoveFreeSpace(s);             /* len=2 → type 5, cross! */
            s = sdscat(s, "!");
            assert(sdslen(s) == 3);
            assert(strcmp(s, "ok!") == 0);
            printf("[PASS] sdsRemoveFreeSpace cross-type 32→5 + arena verify\n");
        }

        /* 26. Chained cross-type: sdsMakeRoomFor then sdsRemoveFreeSpace */
        {
            sds s = sdsnewlen(&ar, "go", 2);       /* type 5 */
            s = sdsMakeRoomFor(s, 500);            /* 5→16 */
            s = sdscat(s, "lang");                 /* uses arena */
            s = sdsRemoveFreeSpace(s);             /* 16→5 (len=5, <32) */
            s = sdscat(s, "pher");                 /* uses arena again */
            assert(strcmp(s, "golangpher") == 0);
            printf("[PASS] chained cross-type: MakeRoomFor + RemoveFreeSpace\n");
        }

        arena_free(&ar);
    }

    /* Clean up */
    arena_free(&a);

    printf("\n=== All arena tests passed! ===\n");
    return 0;
}
