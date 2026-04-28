/* toolcall_parser.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_call_parser.h"

/* 内部辅助：向 slot 追加 arguments 字符串片段 */
static int append_arguments(ToolCallSlot *slot, const char *frag) {
    size_t frag_len = strlen(frag);
    if (frag_len == 0) return 0;

    size_t needed = slot->args_len + frag_len + 1;
    if (needed > slot->args_cap) {
        size_t new_cap = needed * 2;
        char *new_buf = realloc(slot->arguments, new_cap);
        if (!new_buf) return -1; /* OOM */
        slot->arguments = new_buf;
        slot->args_cap = new_cap;
    }
    memcpy(slot->arguments + slot->args_len, frag, frag_len);
    slot->args_len += frag_len;
    slot->arguments[slot->args_len] = '\0';
    return 0;
}

/* 初始化 slot */
static void init_slot(ToolCallSlot *slot, const char *id, const char *name) {
    slot->active = 1;
    free(slot->id);
    free(slot->name);
    slot->id = id ? strdup(id) : strdup("");
    slot->name = name ? strdup(name) : strdup("");
    free(slot->arguments);
    slot->arguments = NULL;
    slot->args_len = 0;
    slot->args_cap = 0;
}

/* 清理一个 slot */
static void free_slot(ToolCallSlot *slot) {
    free(slot->id);
    free(slot->name);
    free(slot->arguments);
    slot->id = slot->name = slot->arguments = NULL;
    slot->active = 0;
    slot->args_len = slot->args_cap = 0;
}

void toolcall_parser_init(ToolCallDeltaParser *parser) {
    memset(parser, 0, sizeof(*parser));
}

void toolcall_parser_free(ToolCallDeltaParser *parser) {
    for (int i = 0; i < MAX_TOOL_CALLS; i++) {
        free_slot(&parser->slots[i]);
    }
    parser->any_active = 0;
    parser->finished = 0;
}

/*
 * 判断当前 chunk 是否意味着流生成结束。
 * 规则：finish_reason 存在或 is_done 标志，都可以视为一个对话轮次结束。
 * 注意：即使 finish_reason 是 "tool_calls" 之外的值（如 "stop"），
 * 只要我们有正在累积的 tool calls，也应该结束并输出。
 */
static int is_end_of_stream(const StreamChunk *chunk) {
    if (chunk->is_done) return 1;
    if (chunk->finish_reason_present && strlen(chunk->finish_reason) > 0) return 1;
    return 0;
}

/*
 * 处理 chunk 中的 tool_calls 增量数组
 * 返回值: 0 成功, -1 内存错误
 */
static int process_toolcall_chunk(ToolCallDeltaParser *parser, const cJSON *tc_array) {
    if (!cJSON_IsArray(tc_array)) return -1;
    int count = cJSON_GetArraySize(tc_array);
    for (int i = 0; i < count; i++) {
        cJSON *tc = cJSON_GetArrayItem(tc_array, i);
        if (!cJSON_IsObject(tc)) continue;

        /* 获取 index */
        cJSON *index_item = cJSON_GetObjectItem(tc, "index");
        if (!cJSON_IsNumber(index_item)) continue;
        int idx = index_item->valueint;
        if (idx < 0 || idx >= MAX_TOOL_CALLS) {
            fprintf(stderr, "tool call index %d out of range\n", idx);
            continue;
        }

        ToolCallSlot *slot = &parser->slots[idx];

        /* 新出现的 tool call? 通过是否有 id 字段来判断 */
        cJSON *id_item = cJSON_GetObjectItem(tc, "id");
        if (cJSON_IsString(id_item)) {
            /* 首次出现，初始化 slot (保存 id 和后续可能出现的 name) */
            cJSON *func = cJSON_GetObjectItem(tc, "function");
            const char *name = NULL;
            if (func) {
                cJSON *name_item = cJSON_GetObjectItem(func, "name");
                if (cJSON_IsString(name_item)) {
                    name = name_item->valuestring;
                }
            }
            init_slot(slot, id_item->valuestring, name);
            parser->any_active = 1;
        }

        /* 如果本 chunk 补充了 name (有可能第一次没有 name) */
        if (!slot->active) continue; /* 不应该发生，但安全起见 */

        cJSON *func = cJSON_GetObjectItem(tc, "function");
        if (func) {
            /* 更新 name (如果此时才出现) */
            cJSON *name_item = cJSON_GetObjectItem(func, "name");
            if (cJSON_IsString(name_item) && (!slot->name || strlen(slot->name) == 0)) {
                free(slot->name);
                slot->name = strdup(name_item->valuestring);
            }

            /* 拼接 arguments 片段 */
            cJSON *args_item = cJSON_GetObjectItem(func, "arguments");
            if (cJSON_IsString(args_item)) {
                if (append_arguments(slot, args_item->valuestring) != 0) {
                    return -1; /* OOM */
                }
            }
        }
    }
    return 0;
}

/*
 * 生成最终的 cJSON 数组，并重置 parser 状态。
 * 返回 NULL 表示内存错误或没有任何 tool call。
 */
static cJSON *build_final_toolcalls(ToolCallDeltaParser *parser) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < MAX_TOOL_CALLS; i++) {
        ToolCallSlot *slot = &parser->slots[i];
        if (!slot->active) continue;

        cJSON *tc_obj = cJSON_CreateObject();
        if (!tc_obj) { cJSON_Delete(arr); return NULL; }

        cJSON_AddStringToObject(tc_obj, "id", slot->id ? slot->id : "");
        cJSON_AddStringToObject(tc_obj, "type", "function"); /* 目前只有 function */

        cJSON *func_obj = cJSON_CreateObject();
        if (!func_obj) { cJSON_Delete(tc_obj); cJSON_Delete(arr); return NULL; }
        cJSON_AddItemToObject(tc_obj, "function", func_obj);

        cJSON_AddStringToObject(func_obj, "name", slot->name ? slot->name : "");
        /* arguments 字段：直接填入累积的字符串 */
        if (slot->arguments && strlen(slot->arguments) > 0) {
            cJSON_AddStringToObject(func_obj, "arguments", slot->arguments);
        } else {
            cJSON_AddStringToObject(func_obj, "arguments", "");
        }

        cJSON_AddItemToArray(arr, tc_obj);
    }

    /* 清理所有槽位，准备下一次使用 */
    for (int i = 0; i < MAX_TOOL_CALLS; i++) {
        free_slot(&parser->slots[i]);
    }
    parser->any_active = 0;
    parser->finished = 1; /* 标记已完成，如果要重用需要重新 init */

    return arr;
}

int feed_toolcall_delta(ToolCallDeltaParser *parser,
                        const StreamChunk *chunk,
                        cJSON **out_calls) {
    *out_calls = NULL;
    if (!parser || !chunk) return -1;
    /* 如果之前已经结束了一次，调用者应该先重置，避免状态混乱 */
    if (parser->finished) {
        /* 简单重置，允许再次使用 (也可以直接返回错误，这里选择自动重置) */
        toolcall_parser_free(parser);
        parser->finished = 0;
    }

    /* 1. 先处理 tool_calls 增量 */
    if (chunk->tool_calls && cJSON_IsArray(chunk->tool_calls)) {
        if (process_toolcall_chunk(parser, chunk->tool_calls) != 0) {
            return -1;
        }
    }

    /* 2. 检查是否到达流结束点 */
    if (is_end_of_stream(chunk)) {
        /* 如果有任何 tool call 被激活，输出完整数组 */
        if (parser->any_active) {
            cJSON *final = build_final_toolcalls(parser);
            if (!final) return -1;
            *out_calls = final;
            return 1;
        } else {
            /* 流结束但从未出现 tool call，清理并返回 0 */
            toolcall_parser_free(parser);
            return 0;
        }
    }

    /* 未结束，继续收集 */
    return 0;
}

void toolcall_parser_reset(ToolCallDeltaParser *p)
{
    toolcall_parser_free(p);
    toolcall_parser_init(p);
}