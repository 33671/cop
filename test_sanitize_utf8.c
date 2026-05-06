/*
 * test_sanitize_utf8.c
 *
 * Comprehensive test suite for sanitize_utf8().
 * Covers valid sequences, every class of invalid sequence, edge cases,
 * and the NULL-pointer guard.
 *
 * Build:
 *   cc -Wall -Wextra -o test_sanitize_utf8 test_sanitize_utf8.c sanitize_utf8.c
 *
 * Or via CMake (see CMakeLists.txt in the project root).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sanitize_utf8.h"

/* ── helpers ──────────────────────────────────────────────────────────── */

static int total_tests = 0;
static int passed_tests = 0;

#define TEST(name, expr)  do {                                          \
    total_tests++;                                                      \
    if (!(expr)) {                                                      \
        fprintf(stderr, "  FAIL  %s  (%s)\n", name, #expr);            \
    } else {                                                            \
        passed_tests++;                                                 \
    }                                                                   \
} while(0)

/* Check that buffer matches expected, byte-for-byte. */
static int buf_eq(const uint8_t *got, size_t got_len,
                  const uint8_t *want, size_t want_len)
{
    if (got_len != want_len) return 0;
    return memcmp(got, want, got_len) == 0;
}

/* Helper: call sanitize_utf8 and compare result to expected. */
static void check(const char *label,
                  const uint8_t *input,  size_t input_len,
                  const uint8_t *expect, size_t expect_len)
{
    uint8_t *buf = malloc(input_len ? input_len : 1);
    if (input_len) memcpy(buf, input, input_len);
    sanitize_utf8(buf, input_len);
    TEST(label, buf_eq(buf, input_len, expect, expect_len));
    free(buf);
}

#define S(str)  ((const uint8_t *)(str)), strlen(str)
#define SB(buf) ((const uint8_t *)(buf)), sizeof(buf)

/* ── valid sequences ──────────────────────────────────────────────────── */

static void test_empty(void)
{
    /* Should not crash and output should be empty. */
    uint8_t buf[1] = {0};
    sanitize_utf8(buf, 0);
    TEST("empty input", 1);
}

static void test_null_ptr(void)
{
    /* The function should tolerate NULL data gracefully. */
    sanitize_utf8(NULL, 0);
    sanitize_utf8(NULL, 10);
    TEST("NULL pointer (len=0 and len>0)", 1);
}

static void test_ascii(void)
{
    const uint8_t in[] = "Hello, World! 123";
    check("pure ASCII", SB(in), SB(in));
}

static void test_ascii_boundaries(void)
{
    /* All bytes from 0x00 to 0x7F are valid ASCII. */
    uint8_t buf[128];
    for (int i = 0; i < 128; i++) buf[i] = (uint8_t)i;
    sanitize_utf8(buf, 128);
    int ok = 1;
    for (int i = 0; i < 128; i++)
        if (buf[i] != (uint8_t)i) { ok = 0; break; }
    TEST("all ASCII bytes 0x00-0x7F preserved", ok);
}

static void test_2byte(void)
{
    /* U+0080 => C2 80, U+07FF => DF BF */
    uint8_t in[]  = {0xC2, 0x80, 0xDF, 0xBF};
    check("2-byte valid U+0080 and U+07FF", SB(in), SB(in));
}

static void test_3byte(void)
{
    /* U+0800 => E0 A0 80, U+FFFF => EF BF BF */
    uint8_t in[]  = {0xE0, 0xA0, 0x80, 0xEF, 0xBF, 0xBF};
    check("3-byte valid U+0800 and U+FFFF", SB(in), SB(in));
}

static void test_4byte(void)
{
    /* U+10000 => F0 90 80 80, U+10FFFF => F4 8F BF BF */
    uint8_t in[]  = {0xF0, 0x90, 0x80, 0x80, 0xF4, 0x8F, 0xBF, 0xBF};
    check("4-byte valid U+10000 and U+10FFFF", SB(in), SB(in));
}

static void test_mixed_valid(void)
{
    /* Mix of ASCII, 2-byte, 3-byte, 4-byte. */
    uint8_t in[] = {
        'A',                                    /* ASCII */
        0xC3, 0xA9,                             /* U+00E9 (é) */
        0xE4, 0xBD, 0xA0,                      /* U+4F60 (你) */
        0xF0, 0x9F, 0x98, 0x80,                /* U+1F600 (😀) */
        '!'
    };
    check("mixed valid sequences", SB(in), SB(in));
}

/* ── invalid sequences ────────────────────────────────────────────────── */

static void test_continuation_as_first_byte(void)
{
    /* 0x80 and 0xBF as standalone first bytes. */
    uint8_t in[]    = {0x80, 0xBF};
    uint8_t expect[]= {'?',  '?'};
    check("continuation byte as first byte", SB(in), SB(expect));
}

static void test_overlong_2byte(void)
{
    /* U+002F encoded as 2-byte overlong: C0 AF (should be just 0x2F). */
    uint8_t in[]    = {0xC0, 0xAF};
    uint8_t expect[]= {'?',  '?'};  /* both bytes replaced (0xC0 illegal head, 0xAF stray cont.) */
    check("overlong 2-byte C0 AF -> all bytes replaced", SB(in), SB(expect));
}

static void test_overlong_2byte_c1(void)
{
    /* U+0000 encoded as C1 80 (overlong of ASCII NUL). */
    uint8_t in[]    = {0xC1, 0x80};
    uint8_t expect[]= {'?',  '?'};
    check("overlong 2-byte C1 80 -> all bytes replaced", SB(in), SB(expect));
}

static void test_overlong_3byte(void)
{
    /* U+002F encoded as 3-byte overlong: E0 80 AF. */
    uint8_t in[]    = {0xE0, 0x80, 0xAF};
    uint8_t expect[]= {'?',  '?', '?'};
    check("overlong 3-byte E0 80 AF -> all bytes replaced", SB(in), SB(expect));
}

static void test_overlong_4byte(void)
{
    /* U+002F encoded as 4-byte overlong: F0 80 80 AF. */
    uint8_t in[]    = {0xF0, 0x80, 0x80, 0xAF};
    uint8_t expect[]= {'?',  '?', '?', '?'};
    check("overlong 4-byte F0 80 80 AF -> all bytes replaced", SB(in), SB(expect));
}

static void test_missing_continuation_2byte(void)
{
    /* 2-byte head at last position – no room for continuation. */
    uint8_t in[]    = {0xC3};
    uint8_t expect[]= {'?'};
    check("truncated 2-byte at end", SB(in), SB(expect));
}

static void test_missing_continuation_3byte(void)
{
    /* 3-byte head followed by only one continuation byte. */
    uint8_t in[]    = {0xE4, 0xBD};
    uint8_t expect[]= {'?',  '?'};
    check("truncated 3-byte (1 of 2 cont.)", SB(in), SB(expect));
}

static void test_missing_continuation_4byte(void)
{
    /* 4-byte head with zero continuation bytes. */
    uint8_t in[]    = {0xF0};
    uint8_t expect[]= {'?'};
    check("truncated 4-byte (0 of 3 cont.)", SB(in), SB(expect));
}

static void test_bad_continuation(void)
{
    /* Continuation byte does not start with 0b10xxxxxx. */
    uint8_t in[]    = {0xC3, 0x00};
    uint8_t expect[]= {'?',  0x00};
    check("bad continuation byte 0x00 after 2-byte head", SB(in), SB(expect));
}

static void test_bad_continuation2(void)
{
    uint8_t in[]    = {0xE4, 0xBD, 0xC0};
    uint8_t expect[]= {'?',  '?', '?'};
    check("bad continuation byte 0xC0 after 3-byte head", SB(in), SB(expect));
}

static void test_surrogate(void)
{
    /* U+D800 encoded as ED A0 80. */
    uint8_t in[]    = {0xED, 0xA0, 0x80};
    uint8_t expect[]= {'?',  '?', '?'};
    check("surrogate U+D800 (ED A0 80) -> all bytes replaced", SB(in), SB(expect));
}

static void test_surrogate_max(void)
{
    /* U+DFFF encoded as ED BF BF. */
    uint8_t in[]    = {0xED, 0xBF, 0xBF};
    uint8_t expect[]= {'?',  '?', '?'};
    check("surrogate U+DFFF (ED BF BF) -> all bytes replaced", SB(in), SB(expect));
}

static void test_codepoint_too_large(void)
{
    /* U+110000 => F4 90 80 80 (above U+10FFFF). */
    uint8_t in[]    = {0xF4, 0x90, 0x80, 0x80};
    uint8_t expect[]= {'?',  '?', '?', '?'};
    check("codepoint > U+10FFFF (F4 90 80 80)", SB(in), SB(expect));
}

static void test_illegal_first_byte_f5(void)
{
    uint8_t in[]    = {0xF5, 0x80, 0x80, 0x80};
    uint8_t expect[]= {'?',  '?', '?', '?'};
    check("illegal first byte 0xF5", SB(in), SB(expect));
}

static void test_illegal_first_byte_ff(void)
{
    uint8_t in[]    = {0xFF};
    uint8_t expect[]= {'?'};
    check("illegal first byte 0xFF", SB(in), SB(expect));
}

static void test_illegal_first_byte_fe(void)
{
    uint8_t in[]    = {0xFE};
    uint8_t expect[]= {'?'};
    check("illegal first byte 0xFE", SB(in), SB(expect));
}

/* ── mixed valid/invalid ──────────────────────────────────────────────── */

static void test_mixed_valid_invalid(void)
{
    /* 'A' + truncated 3-byte + valid é + illegal 0xFF + valid 4-byte. */
    uint8_t in[]    = {'A', 0xE4, 0xBD, 0xC3, 0xA9, 0xFF, 0xF0, 0x9F, 0x98, 0x80};
    /* Tracing:
       - in[0]='A' -> ASCII, keep. i=1
       - in[1]=0xE4 (3-byte head), in[2]=0xBD (ok), in[3]=0xC3 (NOT continuation) -> replace in[1]='?', i=2
       - in[2]=0xBD (continuation byte as first byte) -> replace '?', i=3
       - in[3]=0xC3, in[4]=0xA9 -> valid 2-byte é, i=5
       - in[5]=0xFF -> illegal, replace '?', i=6
       - in[6..9]=0xF0 0x9F 0x98 0x80 -> valid 4-byte 😀, i=10
    */
    uint8_t expect[]= {'A', '?', '?', 0xC3, 0xA9, '?', 0xF0, 0x9F, 0x98, 0x80};
    check("mixed valid and invalid sequences", SB(in), SB(expect));
}

/* ── edge cases ───────────────────────────────────────────────────────── */

static void test_only_continuation_bytes(void)
{
    /* Buffer full of continuation bytes (all illegal). */
    uint8_t in[10];
    uint8_t expect[10];
    for (int i = 0; i < 10; i++) {
        in[i] = 0x80 + (uint8_t)i;
        expect[i] = '?';
    }
    check("only continuation bytes", SB(in), SB(expect));
}

static void test_valid_then_truncated(void)
{
    /* Valid ASCII, then valid 2-byte, then truncated 4-byte. */
    uint8_t in[]    = {'H', 'i', 0xC3, 0xA9, 0xF0, 0x90};
    uint8_t expect[]= {'H', 'i', 0xC3, 0xA9, '?',  '?'};
    check("valid seq followed by truncated 4-byte", SB(in), SB(expect));
}

static void test_e0_min_boundary(void)
{
    /* U+0800 minimum: E0 A0 80. U+07FF (just below) should fail. */
    uint8_t in[]    = {0xE0, 0xA0, 0x80}; /* valid U+0800 */
    check("E0 A0 80 = U+0800 (valid min 3-byte)", SB(in), SB(in));
}

static void test_e0_overlong(void)
{
    /* E0 9F 80 — this decodes to U+07C0 which is < U+0800 → overlong. */
    uint8_t in[]    = {0xE0, 0x9F, 0x80};
    uint8_t expect[]= {'?',  '?', '?'};
    check("E0 9F 80 = U+07C0 (overlong)", SB(in), SB(expect));
}

static void test_f0_min_boundary(void)
{
    /* U+10000 minimum: F0 90 80 80. */
    uint8_t in[]    = {0xF0, 0x90, 0x80, 0x80};
    check("F0 90 80 80 = U+10000 (valid min 4-byte)", SB(in), SB(in));
}

static void test_f0_overlong(void)
{
    /* F0 8F 80 80 — decodes to U+F000 which is < U+10000 → overlong. */
    uint8_t in[]    = {0xF0, 0x8F, 0x80, 0x80};
    uint8_t expect[]= {'?',  '?', '?', '?'};
    check("F0 8F 80 80 (overlong < U+10000)", SB(in), SB(expect));
}

static void test_f4_max_valid(void)
{
    /* U+10FFFF = F4 8F BF BF. */
    uint8_t in[]    = {0xF4, 0x8F, 0xBF, 0xBF};
    check("F4 8F BF BF = U+10FFFF (max valid)", SB(in), SB(in));
}

static void test_f4_just_over(void)
{
    /* F4 90 80 80 = U+110000 > U+10FFFF. */
    uint8_t in[]    = {0xF4, 0x90, 0x80, 0x80};
    uint8_t expect[]= {'?',  '?', '?', '?'};
    check("F4 90 80 80 = U+110000 (too large)", SB(in), SB(expect));
}

static void test_zero_byte_in_middle(void)
{
    /* ASCII NUL (0x00) in the middle should be preserved. */
    uint8_t in[]    = {'a', 0x00, 'b'};
    check("NUL byte in middle", SB(in), SB(in));
}

static void test_all_illegal_first_bytes(void)
{
    /* Test every byte from 0xF5 to 0xFF (illegal first bytes). */
    uint8_t in[11];
    uint8_t expect[11];
    for (int i = 0; i < 11; i++) {
        in[i] = (uint8_t)(0xF5 + i);
        expect[i] = '?';
    }
    check("illegal first bytes 0xF5-0xFF", SB(in), SB(expect));
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("sanitize_utf8 test suite\n");
    printf("========================\n\n");

    test_empty();
    test_null_ptr();
    test_ascii();
    test_ascii_boundaries();
    test_2byte();
    test_3byte();
    test_4byte();
    test_mixed_valid();
    test_continuation_as_first_byte();
    test_overlong_2byte();
    test_overlong_2byte_c1();
    test_overlong_3byte();
    test_overlong_4byte();
    test_missing_continuation_2byte();
    test_missing_continuation_3byte();
    test_missing_continuation_4byte();
    test_bad_continuation();
    test_bad_continuation2();
    test_surrogate();
    test_surrogate_max();
    test_codepoint_too_large();
    test_illegal_first_byte_f5();
    test_illegal_first_byte_ff();
    test_illegal_first_byte_fe();
    test_mixed_valid_invalid();
    test_only_continuation_bytes();
    test_valid_then_truncated();
    test_e0_min_boundary();
    test_e0_overlong();
    test_f0_min_boundary();
    test_f0_overlong();
    test_f4_max_valid();
    test_f4_just_over();
    test_zero_byte_in_middle();
    test_all_illegal_first_bytes();

    printf("\n");
    printf("────────────────────────────────\n");
    printf("  %d / %d tests passed\n", passed_tests, total_tests);
    printf("  %s\n", passed_tests == total_tests ? "ALL PASSED" : "SOME FAILED");
    return (passed_tests == total_tests) ? 0 : 1;
}
