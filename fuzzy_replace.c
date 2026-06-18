/*
 * fuzzy_replace.c — 模糊字符串替换 实现
 *
 * 算法：逐字符扫描 + token 序列匹配，零正则。
 *
 * 匹配规则：
 *   - 水平空白（空格 ' '、制表符 '\t'、回车 '\r'）在 text 中可匹配任意数量/类型。
 *   - 换行 '\n'：old_str 中有 n 个 \n 则 text 中也必须有恰好 n 个 \n。
 *   - 当 old_str 中无 \n 时，向后兼容：\n 仍被当作普通空白匹配。
 */

#include "fuzzy_replace.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>   /* strncasecmp (POSIX) */

/* ──────────────────────────────────────────────
 * 内部：按空白切分 old_str 为 token 数组
 * ────────────────────────────────────────────── */

/*
 * 将 s 按空白字符切分成 tokens，同时判断首尾是否有空白。
 * tokens 数组和每个 token 字符串均从 arena 分配。
 *
 * 另外输出 newline_sep 数组（长度 n_tokens，int 类型），
 * newline_sep[idx] 表示 token[idx-1] 和 token[idx] 之间
 * 的空白中包含多少个 \n（0 表示纯水平空白）。
 * newline_sep[0] 未使用（保留为 0）。
 *
 * 返回 token 数组，*out_count 为 token 数量。
 * 若没有 token（old_str 全空白），*out_count = 0 且返回 NULL。
 */
static sds* tokenize(Arena *a,
                     const char *s,
                     int *out_count,
                     int *leading_ws,
                     int *trailing_ws,
                     int **out_newline_sep)
{
    int slen = (int)strlen(s);
    *out_count = 0;
    *leading_ws = 0;
    *trailing_ws = 0;
    *out_newline_sep = NULL;
    if (slen == 0) return NULL;

    /* 判断首尾空白 */
    *leading_ws = isspace((unsigned char)s[0]) ? 1 : 0;
    *trailing_ws = isspace((unsigned char)s[slen - 1]) ? 1 : 0;

    /* 第一遍：数 token */
    int tok_count = 0;
    int in_token = 0;
    for (int i = 0; i < slen; i++) {
        if (isspace((unsigned char)s[i])) {
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

    /* 分配 token 指针数组和 newline_sep 数组 */
    sds *tokens = (sds*)arena_alloc(a, sizeof(sds) * tok_count);
    int *newline_sep = (int*)arena_alloc(a, sizeof(int) * tok_count);
    memset(newline_sep, 0, sizeof(int) * tok_count);

    /* 第二遍：提取 token，同时检测 token 间隙中的 \n 数量 */
    int idx = 0;
    in_token = 0;
    int tok_start = 0;
    int in_gap = 0;           /* 是否处于 token 间空白区域 */
    int gap_nl_count = 0;     /* 当前间隙中的 \n 数量 */

    for (int i = 0; i <= slen; i++) {
        int ws = (i == slen) || isspace((unsigned char)s[i]);
        if (ws) {
            if (in_token) {
                tokens[idx] = sdsnewlen(a, s + tok_start, i - tok_start);
                idx++;
                in_token = 0;
                in_gap = 1;
                gap_nl_count = 0;
            }
            if (in_gap && i < slen && s[i] == '\n') {
                gap_nl_count++;
            }
        } else {
            if (!in_token) {
                tok_start = i;
                in_token = 1;
                /* 刚从一个空白间隙进入新 token */
                if (in_gap && idx > 0) {
                    newline_sep[idx] = gap_nl_count;
                }
                in_gap = 0;
                gap_nl_count = 0;
            }
        }
    }

    *out_count = tok_count;
    *out_newline_sep = newline_sep;
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
 *   newline_sep     - newline_sep[idx] 为 \n 个数（token[idx-1..idx] 间隙中
 *                      包含多少个 \n，0 表示纯水平空白；索引 0 不使用）
 *   leading_ws      - old_str 是否以空白开头
 *   trailing_ws     - old_str 是否以空白结尾
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
                             const int *newline_sep,
                             int leading_ws,
                             int trailing_ws,
                             int case_sensitive,
                             int *out_start)
{
    int ti = start;

    /* ── 1. 处理 leading whitespace ── */
    if (leading_ws) {
        /* old_str 以空白开头：匹配块必须从当前空白算起 */
        if (ti >= text_len || !isspace((unsigned char)text[ti]))
            return -1;
        *out_start = ti;
        while (ti < text_len && isspace((unsigned char)text[ti]))
            ti++;
    } else {
        /* 跳过所有前导空白 */
        while (ti < text_len && isspace((unsigned char)text[ti]))
            ti++;
        *out_start = ti;
    }

    /* ── 2. 逐个匹配 token ── */
    for (int idx = 0; idx < n_tokens; idx++) {
        if (idx > 0) {
            /*
             * token 之间需要空白。区分两种空白：
             *  - 水平空白 (空格 ' '、制表符 '\t'、回车 '\r')：可以模糊匹配
             *  - 换行 '\n'：old_str 中有则要求 text 中也有且数量一致
             */
            if (newline_sep && newline_sep[idx] > 0) {
                /* old_str 在此处有 \n：消耗水平空白，
                 * 然后精确匹配 newline_sep[idx] 个 \n
                 *（每个 \n 前后可有任意水平空白） */
                int nl_needed = newline_sep[idx];
                for (int k = 0; k < nl_needed; k++) {
                    /* 可选的水平空白 */
                    while (ti < text_len &&
                           (text[ti] == ' ' || text[ti] == '\t' || text[ti] == '\r'))
                        ti++;
                    if (ti >= text_len || text[ti] != '\n')
                        return -1;
                    ti++;  /* 消耗 \n */
                }
                /* \n 之后可能跟随的水平空白 */
                while (ti < text_len &&
                       (text[ti] == ' ' || text[ti] == '\t' || text[ti] == '\r'))
                    ti++;
            } else {
                /* old_str 在此处没有 \n，只有水平空白。
                 * 向后兼容：允许 \n 出现并被消耗 */
                if (ti >= text_len || !isspace((unsigned char)text[ti]))
                    return -1;
                while (ti < text_len && isspace((unsigned char)text[ti]))
                    ti++;
            }
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

    /* ── 3. 处理 trailing whitespace ── */
    if (trailing_ws) {
        while (ti < text_len && isspace((unsigned char)text[ti]))
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
    int *newline_sep = NULL;
    sds *tokens = tokenize(a, old_str, &n_tokens, &leading_ws, &trailing_ws,
                           &newline_sep);

    if (n_tokens == 0) {
        /* old_str 为空或全空白 → 返回原文本副本 */
        return sdsnew(a, text);
    }

    /* 2. 扫描 text，逐字符构建结果 */
    sds result = sdsempty(a);
    int i = 0;
    int replacements = 0;

    while (i < text_len) {
        /*
         * 优化：若 old_str 不以空白开头，只在非空白位置尝试匹配。
         * 否则在空白位置也会调用 try_match_tokens → 跳过空白后匹配成功
         * → 这些空白会从结果中丢失。
         */
        int should_try = leading_ws || !isspace((unsigned char)text[i]);

        if (should_try) {
            int match_start = 0;
            int match_end = try_match_tokens(text, text_len, i,
                                             tokens, n_tokens,
                                             newline_sep,
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
    int *newline_sep = NULL;
    sds *tokens = tokenize(a, old_str, &n_tokens, &leading_ws, &trailing_ws,
                           &newline_sep);

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
        int should_try = leading_ws || !isspace((unsigned char)text[i]);

        if (should_try) {
            int ms = 0;
            int me = try_match_tokens(text, text_len, i,
                                      tokens, n_tokens,
                                      newline_sep,
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
    TEST("leading/trailing whitespace in old_str");
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

static void test_replace_old_all_ws(void)
{
    TEST("old_str is all whitespace");
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
    test_replace_old_all_ws();
    test_replace_empty_old();
    test_find_basic();
    test_find_none();
    test_find_one();
    printf("\n%d / %d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
#endif /* FUZZY_REPLACE_TEST */
