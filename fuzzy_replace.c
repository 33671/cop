/*
 * fuzzy_replace.c — 模糊字符串替换 实现
 *
 * 算法：逐字符扫描 + token 序列匹配，零正则。
 *
 * 匹配规则：
 *   - 水平空白（空格 ' '、制表符 '\t'、回车 '\r'）为分隔符，
 *     在 text 中可模糊匹配任意数量/类型。
 *   - 换行 '\n' 是普通 token，必须逐字精确匹配。
 */

#include "fuzzy_replace.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>   /* strncasecmp (POSIX) */

/* ──────────────────────────────────────────────
 * 内部 helper
 * ────────────────────────────────────────────── */

/* 水平空白：空格、制表符、回车。这些是模糊分隔符。*/
static inline int is_hspace(int c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* ──────────────────────────────────────────────
 * 内部：按水平空白切分 old_str 为 token 数组
 * ────────────────────────────────────────────── */

/*
 * 将 s 按水平空白（' ', '\t', '\r'）切分成 tokens。
 * '\n' 作为普通字符留在 token 中，多个连续 '\n' 各自成 token。
 *
 * tokens 数组和每个 token 字符串均从 arena 分配。
 *
 * 参数：
 *   out_count    - 输出 token 数量
 *   leading_ws   - old_str 是否以水平空白开头
 *   trailing_ws  - old_str 是否以水平空白结尾
 *
 * 返回 token 数组；若没有 token（纯水平空白或空串），*out_count=0 且返回 NULL。
 */
static sds* tokenize(Arena *a,
                     const char *s,
                     int *out_count,
                     int *leading_ws,
                     int *trailing_ws)
{
    int slen = (int)strlen(s);
    *out_count = 0;
    *leading_ws = 0;
    *trailing_ws = 0;
    if (slen == 0) return NULL;

    /* 判断首尾水平空白 */
    *leading_ws  = is_hspace((unsigned char)s[0]) ? 1 : 0;
    *trailing_ws = is_hspace((unsigned char)s[slen - 1]) ? 1 : 0;

    /* 第一遍：数 token。仅水平空白是分隔符；'\n' 是 token 的一部分。*/
    int tok_count = 0;
    int in_token = 0;
    for (int i = 0; i < slen; i++) {
        if (is_hspace((unsigned char)s[i])) {
            in_token = 0;
        } else {
            if (!in_token) {
                tok_count++;
                in_token = 1;
            }
        }
    }
    if (tok_count == 0) {
        *out_count = 0;
        return NULL;
    }

    /* 分配 token 指针数组 */
    sds *tokens = (sds*)arena_alloc(a, sizeof(sds) * tok_count);

    /* 第二遍：提取 token */
    int idx = 0;
    in_token = 0;
    int tok_start = 0;
    for (int i = 0; i <= slen; i++) {
        int ws = (i == slen) || is_hspace((unsigned char)s[i]);
        if (ws) {
            if (in_token) {
                tokens[idx++] = sdsnewlen(a, s + tok_start, i - tok_start);
                in_token = 0;
            }
        } else {
            if (!in_token) {
                tok_start = i;
                in_token = 1;
            }
        }
    }

    *out_count = tok_count;
    return tokens;
}

/* ──────────────────────────────────────────────
 * 内部：在 text 的 start 位置尝试匹配 token 序列
 * ────────────────────────────────────────────── */

/*
 * 参数：
 *   text, text_len  - 原始文本及长度
 *   start           - 当前扫描起始位置
 *   tokens, n_tokens - old_str 拆分出的 token 数组
 *   leading_ws      - old_str 是否以水平空白开头
 *   trailing_ws     - old_str 是否以水平空白结尾
 *   case_sensitive  - 是否区分大小写
 *   out_start       - 输出匹配块起始位置
 *
 * 返回值：
 *   匹配成功 → match_end（匹配块结束位置，不含）
 *   匹配失败 → -1
 */
static int try_match_tokens(const char *text,
                             int text_len,
                             int start,
                             sds *tokens,
                             int n_tokens,
                             int leading_ws,
                             int trailing_ws,
                             int case_sensitive,
                             int *out_start)
{
    int ti = start;

    /* ── 1. 处理 leading whitespace ── */
    if (leading_ws) {
        /* old_str 以水平空白开头：匹配块从此处开始 */
        if (ti >= text_len || !is_hspace((unsigned char)text[ti]))
            return -1;
        *out_start = ti;
        while (ti < text_len && is_hspace((unsigned char)text[ti]))
            ti++;
    } else {
        /* 跳过所有前导水平空白（'\n' 不会被跳过，它是 token）*/
        while (ti < text_len && is_hspace((unsigned char)text[ti]))
            ti++;
        *out_start = ti;
    }

    /* ── 2. 逐个匹配 token ── */
    for (int idx = 0; idx < n_tokens; idx++) {
        if (idx > 0) {
            /* token 之间消耗任意水平空白（'\n' 是 token，不会被消耗）*/
            if (ti < text_len && !is_hspace((unsigned char)text[ti])) {
                /* 两个 token 直接相邻（old_str 中它们被 '\n' 分隔，
                 * 而 '\n' 本身就是 token，所以实际上不会走到这里）*/
            }
            while (ti < text_len && is_hspace((unsigned char)text[ti]))
                ti++;
        }

        size_t tok_len = sdslen(tokens[idx]);
        if (ti + (int)tok_len > text_len)
            return -1;

        int ok;
        if (case_sensitive) {
            ok = (memcmp(text + ti, tokens[idx], tok_len) == 0);
        } else {
#ifdef _WIN32
            ok = (_strnicmp(text + ti, tokens[idx], tok_len) == 0);
#else
            ok = (strncasecmp(text + ti, tokens[idx], tok_len) == 0);
#endif
        }
        if (!ok) return -1;
        ti += (int)tok_len;
    }

    /* ── 3. 处理 trailing horizontal whitespace ── */
    if (trailing_ws) {
        while (ti < text_len && is_hspace((unsigned char)text[ti]))
            ti++;
    }

    return ti;  /* match_end */
}

/* ──────────────────────────────────────────────
 * 公开 API
 * ────────────────────────────────────────────── */

sds fuzzy_replace(Arena *a,
                   const char *text,
                   const char *old_str,
                   const char *new_str,
                   int count,
                   int case_sensitive)
{
    int text_len = (int)strlen(text);

    /* 1. 切分 old_str */
    int n_tokens = 0;
    int leading_ws = 0, trailing_ws = 0;
    sds *tokens = tokenize(a, old_str, &n_tokens, &leading_ws, &trailing_ws);

    if (n_tokens == 0) {
        /* old_str 为空或全水平空白 → 返回原文本副本 */
        return sdsnew(a, text);
    }

    /* 2. 扫描 text，逐字符构建结果 */
    sds result = sdsempty(a);
    int i = 0;
    int replacements = 0;

    while (i < text_len) {
        /*
         * 优化：若 old_str 不以空白开头，只在非水平空白位置尝试匹配。
         * '\n' 不是水平空白，所以不会被跳过。
         */
        int should_try = leading_ws || !is_hspace((unsigned char)text[i]);

        if (should_try) {
            int match_start = 0;
            int match_end = try_match_tokens(text, text_len, i,
                                             tokens, n_tokens,
                                             leading_ws, trailing_ws,
                                             case_sensitive,
                                             &match_start);
            if (match_end >= 0) {
                /* 匹配成功 → 追加 new_str，跳过匹配块 */
                result = sdscat(result, new_str);
                i = match_end;
                replacements++;
                if (count > 0 && replacements >= count) {
                    /* 达到替换上限 → 追加剩余文本 */
                    result = sdscatlen(result, text + i, text_len - i);
                    return result;
                }
                continue;
            }
        }

        /* 未匹配 → 保留当前字符 */
        result = sdscatlen(result, text + i, 1);
        i++;
    }

    return result;
}

sds fuzzy_replace_first(Arena *a,
                         const char *text,
                         const char *old_str,
                         const char *new_str,
                         int case_sensitive)
{
    return fuzzy_replace(a, text, old_str, new_str, 1, case_sensitive);
}

MatchSpan* fuzzy_find(Arena *a,
                       const char *text,
                       const char *old_str,
                       int case_sensitive,
                       int *out_count)
{
    int text_len = (int)strlen(text);
    *out_count = 0;

    int n_tokens = 0;
    int leading_ws = 0, trailing_ws = 0;
    sds *tokens = tokenize(a, old_str, &n_tokens, &leading_ws, &trailing_ws);

    if (n_tokens == 0)
        return NULL;

    /*
     * 用动态数组收集匹配位置。
     * 手动管理动态数组（arena 不支持 free 单个对象，
     * 但 arena_realloc 可以扩缩容）。
     */
    int cap = 8;
    MatchSpan *spans = (MatchSpan*)arena_alloc(a, sizeof(MatchSpan) * cap);
    int cnt = 0;

    int i = 0;
    while (i < text_len) {
        int should_try = leading_ws || !is_hspace((unsigned char)text[i]);

        if (should_try) {
            int ms = 0;
            int me = try_match_tokens(text, text_len, i,
                                      tokens, n_tokens,
                                      leading_ws, trailing_ws,
                                      case_sensitive, &ms);
            if (me >= 0) {
                if (cnt >= cap) {
                    cap *= 2;
                    spans = (MatchSpan*)arena_realloc(a, spans,
                                                      sizeof(MatchSpan) * (cap / 2),
                                                      sizeof(MatchSpan) * cap);
                }
                spans[cnt].start  = ms;
                spans[cnt].finish = me;
                cnt++;
                i = me;
                continue;
            }
        }

        i++;
    }

    *out_count = cnt;
    if (cnt == 0) return NULL;
    return spans;
}

sds fuzzy_find_one(Arena *a,
                    const char *text,
                    const char *old_str,
                    int case_sensitive,
                    int idx)
{
    int count = 0;
    MatchSpan *spans = fuzzy_find(a, text, old_str, case_sensitive, &count);
    if (!spans || idx < 0 || idx >= count)
        return NULL;
    return sdsnewlen(a, text + spans[idx].start,
                        spans[idx].finish - spans[idx].start);
}

/* ──────────────────────────────────────────────
 * 测试 main（编译方式见文末注释）
 * ────────────────────────────────────────────── */
#ifdef FUZZY_REPLACE_TEST
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s ... ", name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while(0)
#define CHECK(cond) do { if (!(cond)) { FAIL("%s", #cond); return; } } while(0)

static void test_replace_basic(void)
{
    TEST("basic replace with extra spaces");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello    world", "hello world", "hi", 0, 1);
    CHECK(strcmp(r, "hi") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_tabs(void)
{
    TEST("replace with tabs as whitespace");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello\t\tworld foo", "hello world", "hi", 0, 1);
    CHECK(strcmp(r, "hi foo") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_multiple(void)
{
    TEST("replace multiple occurrences");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello    world, hello\tworld",
                          "hello world", "hi", 0, 1);
    CHECK(strcmp(r, "hi, hi") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_preserve_prefix(void)
{
    TEST("preserve text before match");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "Name:  John\t  Doe", "John Doe",
                          "Jane Smith", 0, 1);
    CHECK(strcmp(r, "Name:  Jane Smith") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_leading_trailing_ws(void)
{
    TEST("leading/trailing horizontal whitespace in old_str");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "  hello  world  ", "  hello world  ", "X", 0, 1);
    CHECK(strcmp(r, "X") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_case_insensitive(void)
{
    TEST("case insensitive replace");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "HELLO    WORLD", "hello world", "hi", 0, 0);
    CHECK(strcmp(r, "hi") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_count(void)
{
    TEST("replace with count limit");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "a b, a b, a b", "a b", "x", 2, 1);
    CHECK(strcmp(r, "x, x, a b") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_chinese(void)
{
    TEST("replace Chinese characters");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "数据\t\t挖掘 入门", "数据 挖掘", "机器学习", 0, 1);
    CHECK(strcmp(r, "机器学习 入门") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_no_match(void)
{
    TEST("no match returns copy of original");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello world", "xyz abc", "hi", 0, 1);
    CHECK(strcmp(r, "hello world") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_old_all_hspace(void)
{
    TEST("old_str is all horizontal whitespace");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello world", "   \t  ", "X", 0, 1);
    CHECK(strcmp(r, "hello world") == 0);
    arena_free(&a);
    PASS();
}

static void test_replace_empty_old(void)
{
    TEST("old_str is empty");
    Arena a = {0};
    sds r = fuzzy_replace(&a, "hello", "", "X", 0, 1);
    CHECK(strcmp(r, "hello") == 0);
    arena_free(&a);
    PASS();
}

/* ── newline-as-token 行为测试 ── */

static void test_newline_exact_match(void)
{
    TEST("\\n as token: exact count must match");
    Arena a = {0};
    /* text has 3 blank lines, old has 3 \n tokens */
    const char *text = "def hello():\n"
                       "    print(\"Hello!\")\n"
                       "\n"
                       "\n"
                       "\n"
                       "    print(\"World!\")\n";
    /* old includes the 4 \n between the prints (1 code line end + 3 blank lines) */
    const char *old_str = "print(\"Hello!\")\n\n\n\n    print(\"World!\")";
    sds r = fuzzy_replace(&a, text, old_str, "print(\"X\")\n    print(\"Y\")", 0, 1);
    CHECK(strstr(r, "print(\"X\")") != NULL);
    CHECK(strstr(r, "print(\"Y\")") != NULL);
    arena_free(&a);
    PASS();
}

static void test_newline_count_mismatch(void)
{
    TEST("\\n as token: count mismatch causes no-match");
    Arena a = {0};
    /* text has 4 \n between prints, old has only 1 */
    const char *text = "def hello():\n"
                       "    print(\"Hello!\")\n"
                       "\n"
                       "\n"
                       "\n"
                       "    print(\"World!\")\n";
    const char *old_str = "print(\"Hello!\")\n    print(\"World!\")";
    /* 1 \n in old vs 4 in text → no match */
    sds r = fuzzy_replace(&a, text, old_str, "X", 0, 1);
    CHECK(strcmp(r, text) == 0);  /* unchanged */
    arena_free(&a);
    PASS();
}

static void test_newline_no_cross_match(void)
{
    TEST("\\n as token: same-line old won't match across lines");
    Arena a = {0};
    /* old has no \n → won't match hello\nworld */
    sds r = fuzzy_replace(&a, "hello\nworld", "hello world", "hi", 0, 1);
    /* No match because \n is not horizontal whitespace */
    CHECK(strstr(r, "hello\nworld") != NULL);
    arena_free(&a);
    PASS();
}

static void test_find_basic(void)
{
    TEST("fuzzy_find basic");
    Arena a = {0};
    int count = 0;
    MatchSpan *spans = fuzzy_find(&a, "hello    world, hello\tworld",
                                 "hello world", 1, &count);
    CHECK(count == 2);
    CHECK(spans[0].start == 0);
    CHECK(spans[0].finish == 14);
    CHECK(spans[1].start == 16);
    CHECK(spans[1].finish == 27);
    arena_free(&a);
    PASS();
}

static void test_find_none(void)
{
    TEST("fuzzy_find no match");
    Arena a = {0};
    int count = -1;
    MatchSpan *spans = fuzzy_find(&a, "abc def", "xyz", 1, &count);
    CHECK(count == 0);
    CHECK(spans == NULL);
    arena_free(&a);
    PASS();
}

static void test_find_one(void)
{
    TEST("fuzzy_find_one");
    Arena a = {0};
    sds s = fuzzy_find_one(&a, "hello    world, hello\tworld",
                           "hello world", 1, 0);
    CHECK(strcmp(s, "hello    world") == 0);
    sds s2 = fuzzy_find_one(&a, "hello    world, hello\tworld",
                            "hello world", 1, 1);
    CHECK(strcmp(s2, "hello\tworld") == 0);
    arena_free(&a);
    PASS();
}

int main(void)
{
    printf("fuzzy_replace tests:\n");
    test_replace_basic();
    test_replace_tabs();
    test_replace_multiple();
    test_replace_preserve_prefix();
    test_replace_leading_trailing_ws();
    test_replace_case_insensitive();
    test_replace_count();
    test_replace_chinese();
    test_replace_no_match();
    test_replace_old_all_hspace();
    test_replace_empty_old();
    test_newline_exact_match();
    test_newline_count_mismatch();
    test_newline_no_cross_match();
    test_find_basic();
    test_find_none();
    test_find_one();
    printf("\n%d / %d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
#endif /* FUZZY_REPLACE_TEST */
