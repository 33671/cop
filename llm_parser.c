#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include "llm_parser.h"

/* ============================================================================
 * Internal State Machine
 * ============================================================================ */
typedef enum {
    STATE_IDLE,
    STATE_IN_ASSISTANT,
    STATE_ERROR
} ParserState;

typedef struct {
    int index;
    char *id;
    char *func_name;
    char *args_buf;
    size_t args_len;
} ToolCallSlot;

struct LlmParser {
    ParserState state;
    cJSON *history;            /* {"messages": [...]} */
    cJSON *messages_array;

    /* Temporary assistant build area */
    struct {
        cJSON *msg;
        char *content_buf;
        size_t content_len;
        char *reasoning_buf;
        size_t reasoning_len;

        ToolCallSlot *slots;
        int slot_count;
        int slot_cap;

        /* Flags to remember what we've seen in this assistant message */
        int flag_reasoning;
        int flag_content;
        int flag_tool;

        /* Last reported status for empty chunks */
        LlmParserStatus last_status;
    } assistant;

    struct {
        int prompt_tokens;
        int completion_tokens;
        int total_tokens;
        int cached_tokens;
    } last_usage;

    char error_msg[256];
};

/* ============================================================================
 * Static Helpers
 * ============================================================================ */
static void set_error(LlmParser *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error_msg, sizeof(p->error_msg), fmt, ap);
    va_end(ap);
    p->state = STATE_ERROR;
}

/* Append src to dst. Returns new pointer or NULL on OOM (dst is still valid). */
static char *str_append(char *dst, size_t *dst_len, const char *src)
{
    if (!src || !*src) return dst;
    size_t src_len = strlen(src);
    char *tmp = realloc(dst, *dst_len + src_len + 1);
    if (!tmp) return NULL;
    dst = tmp;
    memcpy(dst + *dst_len, src, src_len + 1);
    *dst_len += src_len;
    return dst;
}

static void reset_assistant(LlmParser *p)
{
    if (p->assistant.msg) {
        cJSON_Delete(p->assistant.msg);
        p->assistant.msg = NULL;
    }
    free(p->assistant.content_buf);
    p->assistant.content_buf = NULL;
    p->assistant.content_len = 0;

    free(p->assistant.reasoning_buf);
    p->assistant.reasoning_buf = NULL;
    p->assistant.reasoning_len = 0;

    for (int i = 0; i < p->assistant.slot_count; i++) {
        free(p->assistant.slots[i].id);
        free(p->assistant.slots[i].func_name);
        free(p->assistant.slots[i].args_buf);
    }
    free(p->assistant.slots);
    p->assistant.slots = NULL;
    p->assistant.slot_count = 0;
    p->assistant.slot_cap = 0;

    p->assistant.flag_reasoning = 0;
    p->assistant.flag_content = 0;
    p->assistant.flag_tool = 0;
    p->assistant.last_status = LLM_PARSER_RESPONDING;
}

static ToolCallSlot *get_tool_call_slot(LlmParser *p, int index)
{
    for (int i = 0; i < p->assistant.slot_count; i++) {
        if (p->assistant.slots[i].index == index)
            return &p->assistant.slots[i];
    }
    if (p->assistant.slot_count >= p->assistant.slot_cap) {
        int new_cap = p->assistant.slot_cap ? p->assistant.slot_cap * 2 : 4;
        ToolCallSlot *tmp = realloc(p->assistant.slots, new_cap * sizeof(ToolCallSlot));
        if (!tmp) return NULL;
        p->assistant.slots = tmp;
        p->assistant.slot_cap = new_cap;
    }
    ToolCallSlot *slot = &p->assistant.slots[p->assistant.slot_count++];
    memset(slot, 0, sizeof(*slot));
    slot->index = index;
    return slot;
}

/* Finalize the current assistant message and append to history */
static int finish_assistant(LlmParser *p)
{
    if (p->state != STATE_IN_ASSISTANT) return 0;

    cJSON *msg = p->assistant.msg;
    if (!msg) {
        set_error(p, "finish_assistant: no active message");
        return -1;
    }

    /* content: OpenAI allows null */
    if (p->assistant.content_buf && p->assistant.content_len > 0) {
        cJSON_AddStringToObject(msg, "content", p->assistant.content_buf);
    } else {
        cJSON_AddNullToObject(msg, "content");
    }

    /* reasoning_content (OpenAI extended field) */
    if (p->assistant.reasoning_buf && p->assistant.reasoning_len > 0) {
        cJSON_AddStringToObject(msg, "reasoning_content", p->assistant.reasoning_buf);
    }

    /* tool_calls */
    if (p->assistant.slot_count > 0) {
        cJSON *tc_arr = cJSON_CreateArray();
        for (int i = 0; i < p->assistant.slot_count; i++) {
            ToolCallSlot *s = &p->assistant.slots[i];
            cJSON *tc = cJSON_CreateObject();
            if (s->id) cJSON_AddStringToObject(tc, "id", s->id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON *func = cJSON_CreateObject();
            if (s->func_name) cJSON_AddStringToObject(func, "name", s->func_name);
            cJSON_AddStringToObject(func, "arguments", s->args_buf ? s->args_buf : "");
            cJSON_AddItemToObject(tc, "function", func);
            cJSON_AddItemToArray(tc_arr, tc);
        }
        cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
    }

    cJSON_AddItemToArray(p->messages_array, msg);
    p->assistant.msg = NULL;   /* ownership transferred */
    reset_assistant(p);
    p->state = STATE_IDLE;
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */
LlmParser *llm_parser_create(void)
{
    LlmParser *p = calloc(1, sizeof(LlmParser));
    if (!p) return NULL;

    p->state = STATE_IDLE;
    p->history = cJSON_CreateObject();
    p->messages_array = cJSON_CreateArray();
    cJSON_AddItemToObject(p->history, "messages", p->messages_array);
    p->last_usage.cached_tokens = -1;
    p->assistant.last_status = LLM_PARSER_RESPONDING;
    return p;
}

void llm_parser_destroy(LlmParser *p)
{
    if (!p) return;
    reset_assistant(p);
    cJSON_Delete(p->history);
    free(p);
}

LlmParserStatus llm_parser_add_message(LlmParser *p, const cJSON *msg_obj)
{
    if (!p || !msg_obj) {
        if (p) set_error(p, "null argument");
        return LLM_PARSER_ERR_ARG;
    }
    if (p->state == STATE_ERROR) return LLM_PARSER_ERR_STATE;
    if (p->state == STATE_IN_ASSISTANT) {
        set_error(p, "cannot add message while assistant stream in progress");
        return LLM_PARSER_ERR_STATE;
    }

    cJSON *role = cJSON_GetObjectItemCaseSensitive(msg_obj, "role");
    if (!cJSON_IsString(role) || !role->valuestring) {
        set_error(p, "message missing valid 'role'");
        return LLM_PARSER_ERR_ARG;
    }
    if (strcmp(role->valuestring, "assistant") == 0) {
        set_error(p, "use llm_parser_feed_chunk for assistant messages");
        return LLM_PARSER_ERR_ARG;
    }

    cJSON *copy = cJSON_Duplicate(msg_obj, 1);
    if (!copy) {
        set_error(p, "cJSON_Duplicate failed");
        return LLM_PARSER_ERR_OOM;
    }
    cJSON_AddItemToArray(p->messages_array, copy);
    return LLM_PARSER_IDLE;
}

LlmParserStatus llm_parser_feed_chunk(LlmParser *p, const StreamChunk *chunk)
{
    if (!p || !chunk) {
        if (p) set_error(p, "null argument");
        return LLM_PARSER_ERR_ARG;
    }
    if (p->state == STATE_ERROR) return LLM_PARSER_ERR_STATE;
    if (!chunk->is_valid) {
        set_error(p, "invalid chunk");
        return LLM_PARSER_ERR_ARG;
    }

    /* ------------------------------------------------------------------
     *  End-of-stream signals
     * ------------------------------------------------------------------ */
    if (chunk->is_done || chunk->finish_reason_present) {
        if (p->state == STATE_IN_ASSISTANT) {
            if (finish_assistant(p) != 0) {
                return LLM_PARSER_ERR_OOM; /* error_msg already set */
            }
        }
        if (chunk->usage_present) {
            p->last_usage.prompt_tokens = chunk->prompt_tokens;
            p->last_usage.completion_tokens = chunk->completion_tokens;
            p->last_usage.total_tokens = chunk->total_tokens;
            p->last_usage.cached_tokens = chunk->cached_tokens;
        }
        return LLM_PARSER_FINISHED;
    }

    /* ------------------------------------------------------------------
     *  Role start (new assistant message)
     * ------------------------------------------------------------------ */
    if (chunk->role == ROLE_ASSISTANT) {
        if (p->state == STATE_IN_ASSISTANT) {
            /* Robustness: previous stream didn't finish; force-close it */
            if (finish_assistant(p) != 0) {
                return LLM_PARSER_ERR_OOM;
            }
        }
        p->state = STATE_IN_ASSISTANT;
        p->assistant.msg = cJSON_CreateObject();
        cJSON_AddStringToObject(p->assistant.msg, "role", "assistant");
        if (chunk->id)    cJSON_AddStringToObject(p->assistant.msg, "id", chunk->id);
        if (chunk->model) cJSON_AddStringToObject(p->assistant.msg, "model", chunk->model);
        /* Fall through to process other fields in this same chunk */
    }

    /* ------------------------------------------------------------------
     *  IDLE guard: reject stray deltas
     * ------------------------------------------------------------------ */
    if (p->state == STATE_IDLE) {
        if (chunk->content || chunk->reasoning_content || chunk->tool_calls) {
            set_error(p, "received delta without assistant role start");
            return LLM_PARSER_ERR_STATE;
        }
        return LLM_PARSER_IDLE;
    }

    assert(p->state == STATE_IN_ASSISTANT);

    /* ------------------------------------------------------------------
     *  Tool calls (highest priority for mixed deltas)
     * ------------------------------------------------------------------ */
    if (chunk->tool_calls && cJSON_IsArray(chunk->tool_calls)) {
        cJSON *tc_item = NULL;
        cJSON_ArrayForEach(tc_item, chunk->tool_calls) {
            if (!cJSON_IsObject(tc_item)) continue;

            cJSON *jidx = cJSON_GetObjectItemCaseSensitive(tc_item, "index");
            if (!cJSON_IsNumber(jidx)) continue;
            int idx = (int)jidx->valuedouble;

            ToolCallSlot *slot = get_tool_call_slot(p, idx);
            if (!slot) {
                set_error(p, "oom: tool call slot");
                return LLM_PARSER_ERR_OOM;
            }

            cJSON *jid = cJSON_GetObjectItemCaseSensitive(tc_item, "id");
            if (cJSON_IsString(jid) && jid->valuestring && !slot->id) {
                slot->id = strdup(jid->valuestring);
            }

            cJSON *jfunc = cJSON_GetObjectItemCaseSensitive(tc_item, "function");
            if (cJSON_IsObject(jfunc)) {
                cJSON *jname = cJSON_GetObjectItemCaseSensitive(jfunc, "name");
                if (cJSON_IsString(jname) && jname->valuestring && !slot->func_name) {
                    slot->func_name = strdup(jname->valuestring);
                }
                cJSON *jargs = cJSON_GetObjectItemCaseSensitive(jfunc, "arguments");
                if (cJSON_IsString(jargs) && jargs->valuestring) {
                    char *tmp = str_append(slot->args_buf, &slot->args_len, jargs->valuestring);
                    if (!tmp) {
                        set_error(p, "oom: appending arguments");
                        return LLM_PARSER_ERR_OOM;
                    }
                    slot->args_buf = tmp;
                }
            }
        }
        p->assistant.flag_tool = 1;
        p->assistant.last_status = LLM_PARSER_WRITING_TOOL_CALL;
        return LLM_PARSER_WRITING_TOOL_CALL;
    }

    /* ------------------------------------------------------------------
     *  Reasoning content
     * ------------------------------------------------------------------ */
    if (chunk->reasoning_content && chunk->reasoning_content[0]) {
        char *tmp = str_append(p->assistant.reasoning_buf,
                               &p->assistant.reasoning_len,
                               chunk->reasoning_content);
        if (!tmp) {
            set_error(p, "oom: appending reasoning");
            return LLM_PARSER_ERR_OOM;
        }
        p->assistant.reasoning_buf = tmp;
        p->assistant.flag_reasoning = 1;
        p->assistant.last_status = LLM_PARSER_REASONING;
        return LLM_PARSER_REASONING;
    }

    /* ------------------------------------------------------------------
     *  Normal content
     * ------------------------------------------------------------------ */
    if (chunk->content && chunk->content[0]) {
        char *tmp = str_append(p->assistant.content_buf,
                               &p->assistant.content_len,
                               chunk->content);
        if (!tmp) {
            set_error(p, "oom: appending content");
            return LLM_PARSER_ERR_OOM;
        }
        p->assistant.content_buf = tmp;
        p->assistant.flag_content = 1;
        p->assistant.last_status = LLM_PARSER_RESPONDING;
        return LLM_PARSER_RESPONDING;
    }

    /* ------------------------------------------------------------------
     *  Empty delta while IN_ASSISTANT (e.g. usage-only chunk, or role-only start)
     *  Return the current message's dominant status.
     * ------------------------------------------------------------------ */
    return p->assistant.last_status;
}

const cJSON *llm_parser_get_history(const LlmParser *p)
{
    if (!p) return NULL;
    return p->history;
}

void llm_parser_reset(LlmParser *p)
{
    if (!p) return;
    reset_assistant(p);
    cJSON_Delete(p->history);
    p->history = cJSON_CreateObject();
    p->messages_array = cJSON_CreateArray();
    cJSON_AddItemToObject(p->history, "messages", p->messages_array);
    p->state = STATE_IDLE;
    memset(&p->last_usage, 0, sizeof(p->last_usage));
    p->last_usage.cached_tokens = -1;
    p->error_msg[0] = '\0';
}

const char *llm_parser_get_error(const LlmParser *p)
{
    if (!p) return NULL;
    return p->error_msg[0] ? p->error_msg : NULL;
}

int llm_parser_get_last_usage(const LlmParser *p,
                              int *prompt, int *completion,
                              int *total, int *cached)
{
    if (!p) return -1;
    if (prompt)      *prompt = p->last_usage.prompt_tokens;
    if (completion)  *completion = p->last_usage.completion_tokens;
    if (total)       *total = p->last_usage.total_tokens;
    if (cached)      *cached = p->last_usage.cached_tokens;
    return 0;
}
const char* llm_parser_status_to_str(LlmParserStatus status) {
    switch (status) {
        /* Errors */
        case LLM_PARSER_ERR_OOM:    return "LLM_PARSER_ERR_OOM";
        case LLM_PARSER_ERR_STATE:  return "LLM_PARSER_ERR_STATE";
        case LLM_PARSER_ERR_ARG:    return "LLM_PARSER_ERR_ARG";

        /* Success states */
        case LLM_PARSER_IDLE:              return "LLM_PARSER_IDLE";
        case LLM_PARSER_REASONING:         return "LLM_PARSER_REASONING";
        case LLM_PARSER_RESPONDING:        return "LLM_PARSER_RESPONDING";
        case LLM_PARSER_WRITING_TOOL_CALL: return "LLM_PARSER_WRITING_TOOL_CALL";
        case LLM_PARSER_FINISHED:          return "LLM_PARSER_FINISHED";

        default: return "UNKNOWN_LLM_PARSER_STATUS";
    }
}