/*
 * test_lua.c - Quick test for the embedded Lua interpreter
 *
 * Compile with:
 *   gcc -std=c11 -D_GNU_SOURCE -I. -Ilua -o test_lua test_lua.c cop_lua.c lua/liblua.a -lm
 *
 * Or just: make test_lua && ./test_lua
 */

#include "cop_lua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void test(const char *label, const char *code, int expect_ok, const char *expect_contains) {
    char *output = NULL;
    int status = cop_lua_execute(code, &output);
    int ok = (status == 0) == (expect_ok != 0);
    int contains = !expect_contains || (output && strstr(output, expect_contains));

    printf("%-40s ", label);
    if (ok && contains) {
        printf("✓ PASS\n");
    } else {
        printf("✗ FAIL (status=%d, output=\"%s\")\n", status, output ? output : "(null)");
        failures++;
    }
    free(output);
}

int main(void) {
    /* Basic execution */
    test("print('hello')",       "print('hello')",               1, "hello");
    test("arithmetic",           "print(1+2*3)",                 1, "7");
    test("string ops",           "print(string.upper('hi'))",    1, "HI");
    test("math lib",             "print(math.pi)",               1, "3.14159");
    test("table ops",            "t={1,2,3}; print(#t)",         1, "3");
    test("multi-line",           "for i=1,3 do\nprint(i)\nend",  1, "1");
    test("syntax error",         "print(bad syntax!!!",          0, NULL);
    test("runtime error",        "error('boom')",                0, "boom");

    /* cop.* helpers */
    test("cop.pwd",              "print(cop.pwd())",             1, "/home");
    test("cop.ls",               "print(#cop.ls())",             1, NULL);  /* just check no crash */

    /* Multi-line complex */
    const char *complex =
        "local sum = 0\n"
        "for i = 1, 100 do\n"
        "  sum = sum + i\n"
        "end\n"
        "print('sum 1..100 =', sum)";
    test("complex loop",         complex,                         1, "5050");

    /* State persistence across calls */
    cop_lua_execute("counter = (counter or 0) + 1; print(counter)", NULL);
    cop_lua_execute("counter = (counter or 0) + 1; print(counter)", NULL);
    /* This is a visual check - counter should be 1 then 2 */

    printf("\n%d / %d tests passed.\n", 8 - failures, 8);

    cop_lua_cleanup();
    return failures ? 1 : 0;
}
