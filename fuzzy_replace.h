/*
 * fuzzy_replace.h — 模糊字符串替换（纯字符串扫描，无正则）
 *
 * 依赖 ./cop/sds/ 库（含 arena 分配器的修改版 SDS）。
 *
 * 匹配规则：
 *   old_str 中的连续空白字符（空格、tab、换行等）在 text 中匹配任意
 *   数量 / 类型的空白字符——即忽略空白差异做匹配。
 *   匹配到的区块整体替换为 new_str。
 *
 * 示例：
 *   fuzzy_replace(a, "hello    world", "hello world", "hi", 0, 1)
 *     → "hi"
 *   fuzzy_replace(a, "Name:  John\t  Doe", "John Doe", "Jane Smith", 0, 1)
 *     → "Name:  Jane Smith"
 */

#ifndef FUZZY_REPLACE_H
#define FUZZY_REPLACE_H

#include "sds/sds.h"        /* sds, Arena */

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────
 * 匹配结果位置
 * ────────────────────────────────────────────── */
typedef struct {
    int start;   /* 匹配块起始位置（含） */
    int finish;  /* 匹配块结束位置（不含），不用 end 名避免 libmill 宏冲突 */
} MatchSpan;

/* ──────────────────────────────────────────────
 * 核心函数
 * ────────────────────────────────────────────── */

/*
 * 在 text 中搜索 old_str（忽略空白差异），替换为 new_str。
 *
 * 参数：
 *   a              - Arena 分配器（所有内部分配均走此 arena）
 *   text           - 原始文本（null-terminated）
 *   old_str        - 要匹配的旧字符串（空白灵活）
 *   new_str        - 替换后的字符串
 *   count          - 最大替换次数，0 = 全部替换
 *   case_sensitive - 非 0 表示区分大小写
 *
 * 返回值：
 *   sds 字符串（从 arena 分配），调用者通过 arena_reset / arena_free 回收。
 */
sds fuzzy_replace(Arena *a,
                  const char *text,
                  const char *old_str,
                  const char *new_str,
                  int count,
                  int case_sensitive);

/*
 * 只替换第一个匹配。等价于 fuzzy_replace(a, ..., 1, ...)。
 */
sds fuzzy_replace_first(Arena *a,
                        const char *text,
                        const char *old_str,
                        const char *new_str,
                        int case_sensitive);

/*
 * 查找所有匹配位置，返回 MatchSpan 数组（从 arena 分配）。
 *
 * 参数：
 *   out_count - 输出匹配数量
 *
 * 返回值：
 *   MatchSpan 数组，无匹配时 *out_count = 0 且返回可能为 NULL。
 */
MatchSpan* fuzzy_find(Arena *a,
                      const char *text,
                      const char *old_str,
                      int case_sensitive,
                      int *out_count);

/*
 * 返回匹配到的第 idx 个（从 0 开始）原始子串的副本。
 * 越界返回 NULL。返回值是从 arena 分配的 sds。
 */
sds fuzzy_find_one(Arena *a,
                   const char *text,
                   const char *old_str,
                   int case_sensitive,
                   int idx);

#ifdef __cplusplus
}
#endif

#endif /* FUZZY_REPLACE_H */
