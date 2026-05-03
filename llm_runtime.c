/*
 * llm_runtime.c
 *
 * High-level LLM runtime implementation.
 * Orchestrates stream_client + llm_parser + tool execution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "llm_runtime.h"

/* ============================================================================
 * Internal Structure
 * ============================================================================ */
struct llm_runtime {
    /* Core components */
    stream_client_t *client;
    LlmParser *parser;

    /* Cached config strings (also stored inside stream_client) */
    char *api_key;
    char *model;
    char *api_endpoint;
    char *log_file;

    /* Registered tool handlers */
    struct {
        char name[128];
        llm_tool_fn_t fn;
    } tools[LLM_RUNTIME_MAX_TOOLS];
    int tool_count;

    /* Error state */
    char error_msg[256];
    int has_error;

    /* Cancellation flag */
    volatile int running;
};

/* ============================================================================
 * Helpers
 * ============================================================================ */
static void set_error(llm_runtime_t *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->error_msg, sizeof(rt->error_msg), fmt, ap);
    va_end(ap);
    rt->has_error = 1;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */
llm_runtime_t *llm_runtime_new(const char *api_key, const char *model,
                                const char *api_endpoint, const char *log_file) {
    if (!api_key || !model) return NULL;

    llm_runtime_t *rt = calloc(1, sizeof(llm_runtime_t));
    if (!rt) return NULL;

    rt->api_key        = strdup(api_key);
    rt->model          = strdup(model);
    rt->api_endpoint   = strdup(api_endpoint ? api_endpoint
                             : "https://api.moonshot.cn/v1/chat/completions");
    rt->log_file       = log_file ? strdup(log_file) : NULL;
    rt->running        = 1;

    /* Create stream client */
    rt->client = stream_client_new(api_key, model, api_endpoint, log_file);
    if (!rt->client) {
        set_error(rt, "stream_client_new failed");
        llm_runtime_free(rt);
        return NULL;
    }

    /* Create message parser */
    rt->parser = llm_parser_create();
    if (!rt->parser) {
        set_error(rt, "llm_parser_create failed");
        llm_runtime_free(rt);
        return NULL;
    }

    return rt;
}

void llm_runtime_free(llm_runtime_t *rt) {
    if (!rt) return;

    /* Cancel any active request */
    if (rt->client) stream_client_free(rt->client);
    if (rt->parser) llm_parser_destroy(rt->parser);

    free(rt->api_key);
    free(rt->model);
    free(rt->api_endpoint);
    free(rt->log_file);
    free(rt);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */
void llm_runtime_set_system_message(llm_runtime_t *rt, const char *msg) {
    if (!rt || !msg) return;
    stream_client_set_system_message(rt->client, msg);
}

void llm_runtime_set_temperature(llm_runtime_t *rt, double temp) {
    if (!rt) return;
    stream_client_set_temperature(rt->client, temp);
}

/* ============================================================================
 * Tool Registration
 * ============================================================================ */
int llm_runtime_register_tool(llm_runtime_t *rt, const char *name,
                               llm_tool_fn_t fn) {
    if (!rt || !name || !fn) return -1;
    if (rt->tool_count >= LLM_RUNTIME_MAX_TOOLS) return -1;

    /* Check for duplicates */
    for (int i = 0; i < rt->tool_count; i++) {
        if (strcmp(rt->tools[i].name, name) == 0) return -1;
    }

    strncpy(rt->tools[rt->tool_count].name, name,
            sizeof(rt->tools[rt->tool_count].name) - 1);
    rt->tools[rt->tool_count].name[sizeof(rt->tools[rt->tool_count].name) - 1] = '\0';
    rt->tools[rt->tool_count].fn = fn;
    rt->tool_count++;
    return 0;
}

void llm_runtime_set_tool_schema(llm_runtime_t *rt, const cJSON *schemas) {
    if (!rt) return;
    stream_client_set_tool_schemas(rt->client, schemas);
}

/* ============================================================================
 * Conversation
 * ============================================================================ */
int llm_runtime_add_user_message(llm_runtime_t *rt, const char *text) {
    if (!rt || !text) return -1;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", text);

    LlmParserStatus ret = llm_parser_add_message(rt->parser, msg);
    cJSON_Delete(msg);

    return (ret < 0) ? -1 : 0;
}

/* Add a "{role:\"tool\"}" result message to history. */
static void add_tool_result_to_history(llm_runtime_t *rt,
                                        const char *call_id,
                                        const char *content) {
    cJSON *tool_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);
    cJSON_AddStringToObject(tool_msg, "content", content);
    llm_parser_add_message(rt->parser, tool_msg);
    cJSON_Delete(tool_msg);
}

/* ============================================================================
 * Tool Execution (internal)
 *
 * Given a completed assistant message with tool_calls, execute each tool,
 * add the results to the parser history, and notify via callback.
 *
 * If cancellation is detected mid-way, remaining tools get a
 * "User has cancelled" result to keep the history valid.
 *
 * Returns the number of tools executed, or -1 on error.
 * ============================================================================ */
static int execute_tool_calls(llm_runtime_t *rt,
                               const cJSON *tool_calls_json,
                               llm_runtime_callback_t on_chunk,
                               void *user_data) {
    if (!rt || !tool_calls_json || !cJSON_IsArray(tool_calls_json)) return -1;

    int tc_count = cJSON_GetArraySize(tool_calls_json);
    int executed = 0;

    for (int i = 0; i < tc_count; i++) {
        cJSON *tc      = cJSON_GetArrayItem(tool_calls_json, i);
        cJSON *func    = cJSON_GetObjectItem(tc, "function");
        cJSON *id_item = cJSON_GetObjectItem(tc, "id");

        if (!func || !id_item || !cJSON_IsString(id_item)) continue;

        const char *call_id = id_item->valuestring;
        cJSON *name_item = cJSON_GetObjectItem(func, "name");
        cJSON *args_item = cJSON_GetObjectItem(func, "arguments");

        const char *name = (name_item && cJSON_IsString(name_item))
                           ? name_item->valuestring : "unknown";
        const char *args_str = (args_item && cJSON_IsString(args_item))
                               ? args_item->valuestring : "{}";

        /* ---- Cancellation check before each tool ---- */
        if (llm_runtime_is_cancelled(rt)) {
            /* Still add "User has cancelled" result to keep history valid */
            add_tool_result_to_history(rt, call_id,
                "{\"error\":\"User has cancelled\"}");
            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_TOOL_RESULT,
                         name, NULL, user_data);
            }
            executed++;
            continue;
        }

        /* Look up registered handler */
        llm_tool_fn_t tool_fn = NULL;
        for (int j = 0; j < rt->tool_count; j++) {
            if (strcmp(rt->tools[j].name, name) == 0) {
                tool_fn = rt->tools[j].fn;
                break;
            }
        }

        /* Parse arguments and execute */
        cJSON *args   = cJSON_Parse(args_str);
        cJSON *result;
        if (tool_fn) {
            result = tool_fn(rt, args ? args : cJSON_CreateObject());
        } else {
            result = cJSON_CreateString("Error: unknown tool or tool not registered");
        }
        cJSON_Delete(args);

        /* Serialize result for the content field */
        char *result_str = cJSON_PrintUnformatted(result);

        /* Build tool result message */
        cJSON *tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);
        cJSON_AddStringToObject(tool_msg, "content",
                                result_str ? result_str : "");
        free(result_str);
        cJSON_Delete(result);

        llm_parser_add_message(rt->parser, tool_msg);
        cJSON_Delete(tool_msg);

        /* Notify user */
        if (on_chunk) {
            on_chunk(rt, LLM_RT_EVENT_TOOL_RESULT, name, NULL, user_data);
        }

        executed++;
    }

    return executed;
}

/* ============================================================================
 * Streaming Send (Coroutine)
 * ============================================================================ */
coroutine int llm_runtime_send(llm_runtime_t *rt,
                                const char *user_text,
                                llm_runtime_callback_t on_chunk,
                                void *user_data) {
    if (!rt) return -1;

    /* Reset cancellation flag for this new turn */
    rt->running = 1;
    rt->has_error = 0;
    rt->error_msg[0] = '\0';

    /* ---- Step 1: Add user message to history if provided ---- */
    if (user_text) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", user_text);
        LlmParserStatus st = llm_parser_add_message(rt->parser, msg);
        cJSON_Delete(msg);
        if (st < 0) {
            set_error(rt, "llm_parser_add_message failed");
            return -1;
        }
    }

    /* ---- Step 2-8: Main send-and-tool-loop ---- */
    int loop_count = 0;
    while (loop_count < LLM_RUNTIME_MAX_TOOL_LOOPS) {
        loop_count++;

        /* Check cancellation before starting a new request */
        if (llm_runtime_is_cancelled(rt)) {
            llm_parser_force_finish(rt->parser);
            set_error(rt, "cancelled");
            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
            }
            return -1;
        }

        /* ---- Step 2: Get full messages array from parser history ---- */
        const cJSON *history  = llm_parser_get_history(rt->parser);
        cJSON       *messages = cJSON_GetObjectItem(history, "messages");
        if (!messages) {
            set_error(rt, "parser history missing 'messages' array");
            return -1;
        }

        /* ---- Step 3: Start the HTTP streaming request ---- */
        if (stream_client_start_chat(rt->client, messages) != 0) {
            set_error(rt, "stream_client_start_chat failed");
            return -1;
        }

        /* ---- Step 4: Process all chunks ---- */
        LlmParserStatus last_status   = LLM_PARSER_IDLE;
        int             saw_tool_calls = 0;
        int             stream_was_cancelled = 0;

        StreamChunk chunk;
        while (next_chunk(rt->client, &chunk)) {
            /* Check cancellation mid-stream */
            if (llm_runtime_is_cancelled(rt)) {
                stream_client_cancel(rt->client);
                stream_chunk_cleanup(&chunk);
                stream_was_cancelled = 1;
                break;
            }

            /* Feed chunk to parser */
            LlmParserStatus status = llm_parser_feed_chunk(rt->parser, &chunk);

            if (status < 0) {
                /* Parser error */
                const char *err = llm_parser_get_error(rt->parser);
                set_error(rt, "parser error: %s", err ? err : "unknown");
                if (on_chunk) {
                    on_chunk(rt, LLM_RT_EVENT_ERROR, rt->error_msg,
                             NULL, user_data);
                }
                stream_chunk_cleanup(&chunk);
                break;
            }

            /* Notify status changes */
            if (status != last_status && status != LLM_PARSER_IDLE && on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_STATUS_CHANGE,
                         llm_parser_status_to_str(status), NULL, user_data);
                last_status = status;
            }

            /* Notify reasoning content */
            if (chunk.reasoning_content && on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_REASONING,
                         chunk.reasoning_content, NULL, user_data);
            }

            /* Notify text content */
            if (chunk.content && on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_CONTENT,
                         chunk.content, NULL, user_data);
            }

            /* Track tool calls appearance */
            if (chunk.tool_calls && cJSON_GetArraySize(chunk.tool_calls) > 0) {
                saw_tool_calls = 1;
            }

            /* Notify usage if present */
            if (chunk.usage_present && on_chunk) {
                cJSON *usage = cJSON_CreateObject();
                cJSON_AddNumberToObject(usage, "prompt_tokens",
                                        chunk.prompt_tokens);
                cJSON_AddNumberToObject(usage, "completion_tokens",
                                        chunk.completion_tokens);
                cJSON_AddNumberToObject(usage, "total_tokens",
                                        chunk.total_tokens);
                if (chunk.cached_tokens >= 0) {
                    cJSON_AddNumberToObject(usage, "cached_tokens",
                                            chunk.cached_tokens);
                }
                on_chunk(rt, LLM_RT_EVENT_USAGE, NULL, usage, user_data);
                cJSON_Delete(usage);
            }

            stream_chunk_cleanup(&chunk);
        }

        /* ---- Step 5: Wait for curl thread to finish ---- */
        stream_client_wait_done(rt->client);

        /*
         * If the stream was cancelled mid-way, force-finish any partial
         * assistant message (accumulated content/reasoning is preserved,
         * incomplete tool calls are discarded) and return.
         */
        if (stream_was_cancelled || llm_runtime_is_cancelled(rt)) {
            llm_parser_force_finish(rt->parser);
            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
            }
            return 0;
        }

        /* ---- Step 6: Check for tool calls in the last assistant message ---- */
        if (!saw_tool_calls) break;  /* no tools -> turn complete */

        const cJSON *msgs = cJSON_GetObjectItem(
            llm_parser_get_history(rt->parser), "messages");
        int msg_count = msgs ? cJSON_GetArraySize(msgs) : 0;
        if (msg_count == 0) break;

        cJSON *last = cJSON_GetArrayItem(msgs, msg_count - 1);
        if (!last) break;

        cJSON *role = cJSON_GetObjectItem(last, "role");
        if (!role || !cJSON_IsString(role) ||
            strcmp(role->valuestring, "assistant") != 0) break;

        cJSON *tool_calls_json = cJSON_GetObjectItem(last, "tool_calls");
        if (!tool_calls_json || !cJSON_IsArray(tool_calls_json) ||
            cJSON_GetArraySize(tool_calls_json) == 0) break;

        /* ---- Step 7: Notify tool calls and execute them ---- */
        if (on_chunk) {
            on_chunk(rt, LLM_RT_EVENT_TOOL_CALLS, NULL,
                     tool_calls_json, user_data);
        }

        int executed = execute_tool_calls(rt, tool_calls_json,
                                          on_chunk, user_data);
        if (executed < 0) {
            set_error(rt, "tool execution failed");
            return -1;
        }

        /* If cancellation was requested during tool execution, don't loop */
        if (llm_runtime_is_cancelled(rt)) {
            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
            }
            return 0;
        }

        /* ---- Step 8: loop back to send another request ---- */
    }

    /* ---- Step 9: Notify completion ---- */
    if (on_chunk) {
        on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
    }

    return 0;
}

/* ============================================================================
 * Utilities
 * ============================================================================ */
const cJSON *llm_runtime_get_history(const llm_runtime_t *rt) {
    if (!rt) return NULL;
    return llm_parser_get_history(rt->parser);
}

void llm_runtime_reset(llm_runtime_t *rt) {
    if (!rt) return;
    llm_parser_reset(rt->parser);
    rt->has_error = 0;
    rt->error_msg[0] = '\0';
}

void llm_runtime_cancel(llm_runtime_t *rt) {
    if (!rt) return;
    rt->running = 0;
    stream_client_cancel(rt->client);
}

const char *llm_runtime_get_error(const llm_runtime_t *rt) {
    if (!rt || !rt->has_error) return NULL;
    return rt->error_msg;
}

const char *llm_runtime_get_state_string(const llm_runtime_t *rt) {
    if (!rt || !rt->client) return "null";
    return stream_client_get_state_string(rt->client);
}

int llm_runtime_is_cancelled(const llm_runtime_t *rt) {
    return rt ? !rt->running : 1;
}
