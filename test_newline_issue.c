#include "fuzzy_replace.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("%s", #cond); return; } } while(0)

/* old="print(\"Hello!\")\n    print(\"World!\")" — one \n,
 * text has 4 \n between them → should NOT match (strict \n count) */
static void test_strict_newline_count(void)
{
    TEST("strict newline count — extra blank lines cause no-match");
    Arena a = {0};
    const char *text = 
        "def hello():\n"
        "    print(\"Hello!\")\n"
        "\n"
        "\n"
        "\n"
        "    print(\"World!\")\n"
        "    return 42\n";
    /* old has 1 newline, but text has 4 → should not match */
    const char *old_str = "print(\"Hello!\")\n    print(\"World!\")";
    sds r = fuzzy_replace(&a, text, old_str, "print(\"X\")\n    print(\"Y\")", 0, 1);
    /* Should be unchanged since no match */
    CHECK(strstr(r, "print(\"Hello!\")") != NULL);
    CHECK(strstr(r, "print(\"World!\")") != NULL);
    arena_free(&a);
    PASS();
}

/* old includes all 4 \n → should match and replace */
static void test_exact_newline_match(void)
{
    TEST("exact newline match — old includes all blank lines");
    Arena a = {0};
    const char *text = 
        "def hello():\n"
        "    print(\"Hello!\")\n"
        "\n"
        "\n"
        "\n"
        "    print(\"World!\")\n"
        "    return 42\n";
    /* old has 4 \n — matches text exactly */
    const char *old_str = "print(\"Hello!\")\n\n\n\n    print(\"World!\")";
    sds r = fuzzy_replace(&a, text, old_str, "print(\"X\")\n    print(\"Y\")", 0, 1);
    printf("Result:\n---\n%s---\n", r);
    CHECK(strstr(r, "print(\"X\")") != NULL);
    CHECK(strstr(r, "print(\"Y\")") != NULL);
    /* The 3 blank lines were in old_str, so they're replaced too */
    CHECK(strstr(r, "return 42") != NULL);
    arena_free(&a);
    PASS();
}

/* No \n in old → \n in text should still match (backward compat) */
static void test_backward_compat_no_nl(void)
{
    TEST("backward compat: old without \\n still matches \\n in text");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello\nworld", "hello world", "hi", 0, 1);
    CHECK(strcmp(r, "hi") == 0);
    arena_free(&a);
    PASS();
}

/* Basic same-line match still works */
static void test_basic_same_line(void)
{
    TEST("basic same-line replace with extra spaces");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello    world", "hello world", "hi", 0, 1);
    CHECK(strcmp(r, "hi") == 0);
    arena_free(&a);
    PASS();
}

int main(void)
{
    printf("fuzzy_replace newline-aware tests:\n");
    test_strict_newline_count();
    test_exact_newline_match();
    test_backward_compat_no_nl();
    test_basic_same_line();
    printf("\n%d / %d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
