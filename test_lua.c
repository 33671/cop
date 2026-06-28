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

    /* SQLite tests */
    test("sqlite open memory",
         "local db = cop.sqlite_open(); print(tostring(db))",
         1, "sqlite3[");
    test("sqlite create table",
         "local db = cop.sqlite_open();"
         "local r = cop.sqlite_exec(db, 'CREATE TABLE t(x)');"
         "print(r.rows_affected)",
         1, "0");
    test("sqlite insert+query",
         "local db = cop.sqlite_open();"
         "cop.sqlite_exec(db, 'CREATE TABLE t(x)');"
         "cop.sqlite_exec(db, 'INSERT INTO t VALUES(42)');"
         "local rows = cop.sqlite_get(db, 'SELECT * FROM t');"
         "print(#rows, rows[1].x)",
         1, "42");
    test("sqlite get_one",
         "local db = cop.sqlite_open();"
         "cop.sqlite_exec(db, 'CREATE TABLE t(x)');"
         "cop.sqlite_exec(db, 'INSERT INTO t VALUES(7)');"
         "local r = cop.sqlite_get_one(db, 'SELECT * FROM t');"
         "print(r.x)",
         1, "7");
    test("sqlite get_one nil",
         "local db = cop.sqlite_open();"
         "cop.sqlite_exec(db, 'CREATE TABLE t(x)');"
         "local r = cop.sqlite_get_one(db, 'SELECT * FROM t WHERE x=999');"
         "print(r)",
         1, "nil");
    test("sqlite close",
         "local db = cop.sqlite_open();"
         "cop.sqlite_close(db);"
         "print('ok')",
         1, "ok");
    test("sqlite multiple types",
         "local db = cop.sqlite_open();"
         "cop.sqlite_exec(db, 'CREATE TABLE t(i,f,s)');"
         "cop.sqlite_exec(db, [[INSERT INTO t VALUES(1,3.14,'hello')]]);"
         "local r = cop.sqlite_get_one(db, 'SELECT * FROM t');"
         "print(r.i, r.f, r.s)",
         1, "1");
    test("sqlite error handling",
         "local db = cop.sqlite_open();"
         "local ok, err = cop.sqlite_exec(db, 'BAD SQL');"
         "print(ok, err)",
         1, "nil");

    /* State persistence across calls */
    cop_lua_execute("counter = (counter or 0) + 1; print(counter)", NULL);
    cop_lua_execute("counter = (counter or 0) + 1; print(counter)", NULL);
    /* This is a visual check - counter should be 1 then 2 */

    printf("\n%d / %d tests passed.\n", 16 - failures, 16);

    cop_lua_cleanup();
    return failures ? 1 : 0;
}
