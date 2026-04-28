/*
 * toolcall_parser.h / toolcall_parser.c
 *
 * 用于流式解析 OpenAI 兼容的 tool_calls 增量数据，
 * 将每个 chunk 中的片段累积，在检测到结束时输出完整的工具调用 JSON 数组。
 *
 * 编译: 需链接 cJSON
 */

#ifndef TOOLCALL_PARSER_H
#define TOOLCALL_PARSER_H

#include "cjson/cJSON.h"
#include "openai_sse_parser.h"  // 替换为你的 StreamChunk 头文件路径

/* 最多支持同时出现的并行 tool call index */
#define MAX_TOOL_CALLS 256

/* 单个工具调用的累积状态 */
typedef struct {
    int active;             /* 1 表示该 index 已激活 */
    char *id;               /* tool call id, 从第一个出现 id 的 chunk 复制 */
    char *name;             /* function name, 同上 */
    char *arguments;        /* 累积的 arguments 字符串片段 (未转义，就是服务器发来的原字符串) */
    size_t args_len;        /* 当前 arguments 长度 (不含 '\0') */
    size_t args_cap;        /* arguments 缓冲区容量 */
} ToolCallSlot;

/* 解析器的完整上下文 */
typedef struct {
    ToolCallSlot slots[MAX_TOOL_CALLS]; /* 按 index 索引 */
    int any_active;                     /* 是否有任何激活的 slot */
    int finished;                       /* 是否已经完成过一次输出，防止重复使用 */
} ToolCallDeltaParser;

/* 初始化解析器 (使用前调用一次) */
void toolcall_parser_init(ToolCallDeltaParser *parser);

/*
 * 喂入一个 chunk，尝试产出完整的工具调用数组。
 *
 * chunk    : 上层解析好的 StreamChunk 指针
 * out_calls: 输出参数。当函数返回 1 时，*out_calls 指向新生成的
 *            cJSON 数组 (类型为 cJSON_Array)，包含所有累积的完整工具调用。
 *            调用者负责 cJSON_Delete(*out_calls) 释放。
 *            当返回 0 时，*out_calls 为 NULL。
 *
 * 返回值:
 *   1 -> 工具调用已完整，out_calls 有效 (即使数组为空也可能返回 1，
 *         但若从未出现任何 tool_calls 数据，返回 0)
 *   0 -> 尚未完成，需要继续喂流，或流中没有工具调用 (此时 out_calls = NULL)
 *  -1 -> 错误 (例如内存不足)
 */
int feed_toolcall_delta(ToolCallDeltaParser *parser,
                        const StreamChunk *chunk,
                        cJSON **out_calls);

/* 释放解析器内部动态内存 (当不再使用或出现错误后调用) */
void toolcall_parser_free(ToolCallDeltaParser *parser);

void toolcall_parser_reset(ToolCallDeltaParser *parser);
#endif /* TOOLCALL_PARSER_H */