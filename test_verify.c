#include "fuzzy_replace.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *text = 
        "def hello():\n"
        "    print(\"Hello!\")\n"
        "\n"
        "\n"
        "\n"
        "    print(\"World!\")\n"
        "    return 42\n";

    /* Test 1: old has 1 \n, text has 4 → should NOT match */
    printf("=== Test 1: old=1\\n, text=4\\n (should NOT match) ===\n");
    {
        Arena a = {0};
        int count = 0;
        const char *old_str = "print(\"Hello!\")\n    print(\"World!\")";
        MatchSpan *spans = fuzzy_find(&a, text, old_str, 1, &count);
        printf("Match count: %d (expected 0)\n", count);
        if (count == 0) printf("  ✓ Correct: no match (strict \\n count)\n");
        else printf("  ✗ BUG: matched when it shouldn't\n");
        arena_free(&a);
    }

    /* Test 2: old has 4 \n → should match */
    printf("\n=== Test 2: old=4\\n, text=4\\n (should match) ===\n");
    {
        Arena a = {0};
        int count = 0;
        const char *old_str = "print(\"Hello!\")\n\n\n\n    print(\"World!\")";
        MatchSpan *spans = fuzzy_find(&a, text, old_str, 1, &count);
        printf("Match count: %d (expected 1)\n", count);
        if (count == 1) {
            printf("  Span: [%d, %d)\n", spans[0].start, spans[0].finish);
            printf("  Matched text: \"%.*s\"\n", 
                   spans[0].finish - spans[0].start, text + spans[0].start);
            printf("  ✓ Correct\n");
        } else printf("  ✗ Unexpected\n");
        arena_free(&a);
    }

    /* Test 3: old has no \n, text has \n → backward compat */
    printf("\n=== Test 3: old=no\\n, text=has\\n (backward compat) ===\n");
    {
        Arena a = {0};
        int count = 0;
        MatchSpan *spans = fuzzy_find(&a, "hello\nworld", "hello world", 1, &count);
        printf("Match count: %d (expected 1)\n", count);
        if (count == 1) printf("  ✓ Correct: backward compatible\n");
        else printf("  ✗ Unexpected\n");
        arena_free(&a);
    }

    /* Test 4: simulate tool_edit — replace with exact \n count */
    printf("\n=== Test 4: simulate tool_edit with exact \\n ===\n");
    {
        Arena a = {0};
        const char *old_str = "print(\"Hello!\")\n\n\n\n    print(\"World!\")";
        const char *new_str = "print(\"X\")\n    print(\"Y\")";
        sds r = fuzzy_replace(&a, text, old_str, new_str, 0, 1);
        printf("Result:\n---\n%s---\n", r);
        if (strstr(r, "print(\"X\")") && strstr(r, "print(\"Y\")") && strstr(r, "return 42")) {
            printf("  ✓ Correct replacement\n");
        } else {
            printf("  ✗ Bad replacement\n");
        }
        arena_free(&a);
    }

    return 0;
}
