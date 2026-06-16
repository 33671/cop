/*
 * agent_loop.c
 *
 * Core agent turn loop: sends messages, processes streaming responses,
 * detects and executes tool calls.  Formerly the body of llm_runtime_send().
 *
 * Internal module - not part of the public API.
 */

#include "llm_runtime_internal.h"
#include "agent_loop.h"
#include "tool_functions.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Tool Result Helper
 * ============================================================================ */

/*
 * Add a tool result message to history.
 * content is the value for the "content" field - for text results it's
 * a plain string; for image_url results it should be the JSON string {"url":"..."}.
 * Returns 0 on success, -1 on error.
 */
static int add_tool_result_to_history(llm_runtime_t *rt,
                                        const char *call_id,
                                        const char *content) {
    cJSON *tool_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);
    cJSON_AddStringToObject(tool_msg, "content", content);
    LlmParserStatus st = llm_parser_add_message(rt->parser, tool_msg);
    cJSON_Delete(tool_msg);
    if (st < 0) {
        /* Force-finish any stuck assistant state and retry once */
        llm_parser_force_finish(rt->parser);
        tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);
        cJSON_AddStringToObject(tool_msg, "content", content);
        st = llm_parser_add_message(rt->parser, tool_msg);
        cJSON_Delete(tool_msg);
    }
    return (st < 0) ? -1 : 0;
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
    debug_log("[debug] execute_tool_calls: %d total tool calls\n", tc_count);

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
        debug_log("[debug]   tool[%d/%d]: %s id=%s\n", i+1, tc_count, name, call_id);

        /* ---- Cancellation check before each tool ---- */
        if (llm_runtime_is_cancelled(rt)) {
            /* Still add "User has cancelled" result to keep history valid */
            add_tool_result_to_history(rt, call_id,
                "{\"error\":\"User has cancelled\"}");
            if (on_chunk) {
                cJSON *info = cJSON_CreateObject();
                cJSON_AddStringToObject(info, "name", name);
                cJSON_AddStringToObject(info, "preview", "cancelled");
                on_chunk(rt, LLM_RT_EVENT_TOOL_RESULT, NULL, info, user_data);
                cJSON_Delete(info);
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
        cJSON *args = cJSON_Parse(args_str);
        cJSON *result;
        if (args == NULL) {
            /* JSON parse failed - report error, don't call tool */
            const char *err_ptr = cJSON_GetErrorPtr();
            fprintf(stderr,
                    "\n  \033[1;31m[error] tool '%s' - invalid JSON arguments\033[0m\n"
                    "  \033[90m%s\033[0m\n"
                    "  \033[90mparse error at: %s\033[0m\n\n",
                    name ? name : "?",
                    args_str ? args_str : "(null)",
                    err_ptr ? err_ptr : "unknown");
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type", "text");
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf),
                     "error: tool call '%s' has invalid JSON arguments; "
                     "parse error near: %.100s",
                     name ? name : "?",
                     err_ptr ? err_ptr : "unknown");
            cJSON_AddStringToObject(result, "text", errbuf);
        } else if (tool_fn) {
            result = tool_fn(rt, args);
        } else {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type", "text");
            cJSON_AddStringToObject(result, "text",
                "Error: unknown tool or tool not registered");
        }
        if (args) cJSON_Delete(args);

        /*
         * Tool functions return one of two valid JSON formats:
         *   1. {"type":"text", "text":"Output:\nxxx\nExit_code:0"}
         *   2. {"type":"image_url", "image_url":{"url":"data:image/png;base64,..."}}
         *
         * Build the tool result message's content field accordingly:
         *   - type=text     → content = the "text" string value
         *   - type=image_url → content = the "image_url" json object
         */
        cJSON *type_item = cJSON_GetObjectItem(result, "type");
        const char *type_str = (type_item && cJSON_IsString(type_item))
                               ? type_item->valuestring : NULL;

        /* Build tool result message */
        cJSON *tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);

        if (type_str && strcmp(type_str, "text") == 0) {
            cJSON *text_item = cJSON_GetObjectItem(result, "text");
            const char *text_str = (text_item && cJSON_IsString(text_item))
                                   ? text_item->valuestring : "";
            cJSON_AddStringToObject(tool_msg, "content", text_str);
        } else if (type_str && strcmp(type_str, "image_url") == 0) {
            cJSON *img_obj = cJSON_GetObjectItem(result, "image_url");
            if (img_obj) {
                cJSON_AddItemToObject(tool_msg, "content",
                                      cJSON_Duplicate(img_obj, 1));
            } else {
                cJSON_AddStringToObject(tool_msg, "content", "{}");
            }
        } else {
            /* Unknown format - serialize whole result as fallback */
            char *result_str = cJSON_PrintUnformatted(result);
            cJSON_AddStringToObject(tool_msg, "content",
                                    result_str ? result_str : "");
            free(result_str);
        }
        cJSON_Delete(result);

        llm_parser_add_message(rt->parser, tool_msg);

        /* Build tool result preview for callback */
        if (on_chunk) {
            const char *preview_text = "";
            cJSON *preview_item = cJSON_GetObjectItem(tool_msg, "content");
            if (preview_item && cJSON_IsString(preview_item)) {
                preview_text = preview_item->valuestring;
            }

            /* Truncate to first 500 chars / first 10 lines */
            char preview[640];
            size_t plen = strlen(preview_text);
            int lines = 0;
            size_t e = 0;
            for (size_t i = 0; i < plen && i < 500 && lines < 10; i++) {
                preview[e++] = preview_text[i];
                if (preview_text[i] == '\n') lines++;
            }
            if (e < plen) {
                memcpy(preview + e, "...", 3);
                e += 3;
            }
            preview[e] = '\0';

            cJSON *info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", name);
            cJSON_AddStringToObject(info, "preview", preview);
            on_chunk(rt, LLM_RT_EVENT_TOOL_RESULT, NULL, info, user_data);
            cJSON_Delete(info);
        }

        cJSON_Delete(tool_msg);
        executed++;

    }

    return executed;
}

/* ============================================================================
 * Agent Loop - Main Turn Execution
 * ============================================================================ */

int agent_loop_run(llm_runtime_t *rt,
                   const char *user_text,
                   llm_runtime_callback_t on_chunk,
                   void *user_data) {
    if (!rt) return -1;

    debug_log("\n[debug] ===== agent_loop_run ENTER =====\n");

    /* Reset cancellation flag for this new turn */
    rt->running = 1;
    rt->has_error = 0;
    rt->error_msg[0] = '\0';

    /* ---- Step 1: Add user message to history if provided ---- */
    if (user_text) {
        debug_log("[debug] Step 1: adding user message: \"%.60s\"\n", user_text);
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", user_text);
        LlmParserStatus st = llm_parser_add_message(rt->parser, msg);
        if (st < 0) {
            /* If previous assistant stream wasn't properly finished
             * (e.g. Ctrl+C during streaming), force-finish and retry. */
            debug_log("[debug] Step 1: add_message failed (st=%d err=%s), calling force_finish+retry\n",
                     st, llm_parser_get_error(rt->parser) ? llm_parser_get_error(rt->parser) : "?");
            llm_parser_force_finish(rt->parser);
            st = llm_parser_add_message(rt->parser, msg);
            debug_log("[debug] Step 1: retry after force_finish → st=%d\n", st);
        }
        cJSON_Delete(msg);
        if (st < 0) {
            debug_log("[debug] add_message FAILED after force_finish retry, returning -1\n");
            printf("\nllm_parser_add_message error:%s\n",llm_parser_get_error(rt->parser));
            set_error(rt, "llm_parser_add_message failed");
            return -1;
        }
    }

    /* ---- Step 2-8: Main send-and-tool-loop ---- */
    int loop_count = 0;
    while (loop_count < LLM_RUNTIME_MAX_TOOL_LOOPS) {
        loop_count++;
        debug_log("\n[debug] === tool loop iteration %d/%d ===\n", loop_count, LLM_RUNTIME_MAX_TOOL_LOOPS);
        rt->usage_seen = 0;  /* reset per-iteration usage */
        int64_t step_start = now();  /* for TPS computation */

        /* ---- Trim arenas after the first step (free excess regions
         * from the previous tool-loop iteration).  The parser's arena
         * and tool-function arena have already been reset by their
         * respective cleanup paths - this discards unused regions. ---- */
        if (loop_count > 1) {
            llm_parser_trim(rt->parser);
            tool_arena_trim();
        }

        /* Check cancellation before starting a new request */
        if (llm_runtime_is_cancelled(rt)) {
            debug_log("\n[debug] tool loop iter=%d: cancelled at top of loop, stopping\n", loop_count);
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
            debug_log("[debug] history missing 'messages' array, returning -1\n");
            set_error(rt, "parser history missing 'messages' array");
            return -1;
        }

        /* ---- Step 3: Start the HTTP streaming request ---- */
        debug_log("[debug] Step 3: stream_client_start_chat (msgs=%d)\n",
                 messages ? cJSON_GetArraySize(messages) : 0);
        if (stream_client_start_chat(rt->client, messages) != 0) {
            debug_log("[debug] stream_client_start_chat FAILED, returning -1\n");
            set_error(rt, "stream_client_start_chat failed");

            /*
             * If this is a fresh turn (the last message is "user" and
             * was just added at Step 1), pop it from history.  The user
             * shouldn't have to see the same message twice when retrying.
             */
            int mcount = cJSON_GetArraySize(messages);
            if (mcount > 0) {
                cJSON *last = cJSON_GetArrayItem(messages, mcount - 1);
                cJSON *role = cJSON_GetObjectItem(last, "role");
                if (role && cJSON_IsString(role) &&
                    strcmp(role->valuestring, "user") == 0) {
                    llm_parser_pop_last_message(rt->parser);
                }
            }

            return -1;
        }

        /* ---- Step 4: Process all chunks ---- */
        LlmParserStatus last_status   = LLM_PARSER_IDLE;
        int             saw_tool_calls = 0;
        int             stream_was_cancelled = 0;
        int             chunk_count = 0;

        StreamChunk chunk;
        debug_log("[debug] Step 4: entering chunk loop\n");
        while (next_chunk(rt->client, &chunk)) {
            chunk_count++;
            /* Check cancellation mid-stream */
            if (llm_runtime_is_cancelled(rt)) {
                debug_log("[debug] Step 4: cancelled during streaming (chunk %d)\n", chunk_count);
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
                debug_log("[debug] Step 4: PARSER ERROR at chunk %d: st=%d err=%s\n",
                         chunk_count, status, err ? err : "?");
                set_error(rt, "parser error: %s", err ? err : "unknown");
                if (on_chunk) {
                    on_chunk(rt, LLM_RT_EVENT_ERROR, rt->error_msg,
                             NULL, user_data);
                }
                stream_chunk_cleanup(&chunk);
                break;
            }

            /* Notify status changes */
            if (on_chunk) {
                int do_notify = (status != last_status && status != LLM_PARSER_IDLE);
                if (do_notify || status == LLM_PARSER_WRITING_TOOL_CALL) {
                    cJSON *sdata = NULL;
                    if (status == LLM_PARSER_WRITING_TOOL_CALL) {
                        /* Attach in-progress tool call preview */
                        char preview[4096];
                        llm_parser_get_tool_preview(rt->parser,
                                                    preview, sizeof(preview));
                        if (preview[0]) {
                            sdata = cJSON_CreateObject();
                            cJSON_AddStringToObject(sdata, "preview", preview);
                        }
                    }
                    on_chunk(rt, LLM_RT_EVENT_STATUS_CHANGE,
                             llm_parser_status_to_str(status),
                             sdata, user_data);
                    if (sdata) cJSON_Delete(sdata);
                    last_status = status;
                }
            }

            /* Notify reasoning content */
            if (chunk.reasoning_content && chunk.reasoning_content[0] && on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_REASONING,
                         chunk.reasoning_content, NULL, user_data);
            }

            /* Notify text content */
            if (chunk.content && chunk.content[0] && on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_CONTENT,
                         chunk.content, NULL, user_data);
            }

            /* Track tool calls appearance and collect usage (fire via accessors later) */
            if ((chunk.tool_calls && cJSON_GetArraySize(chunk.tool_calls) > 0) ||
                (chunk.finish_reason_present &&
                 strcmp(chunk.finish_reason, "tool_calls") == 0)) {
                saw_tool_calls = 1;
            }

            /* Collect usage from last chunk; fire after streaming ends. */
            if (chunk.usage_present) {
                rt->usage_seen       = 1;
                rt->usage_prompt     = chunk.prompt_tokens;
                rt->usage_completion = chunk.completion_tokens;
                rt->usage_total      = chunk.total_tokens;
                rt->usage_cached     = chunk.cached_tokens;
                rt->usage_elapsed_ms = now() - step_start;
            }

            stream_chunk_cleanup(&chunk);
        }

        debug_log("[debug] Step 4: chunk loop exited. chunk_count=%d saw_tool_calls=%d cancelled=%d\n",
                 chunk_count, saw_tool_calls, stream_was_cancelled);

        /* ---- Step 5: Wait for curl thread to finish ---- */
        stream_client_wait_done(rt->client);
        debug_log("[debug] Step 5: stream_client_wait_done done, client_state=%s\n",
                 stream_client_get_state_string(rt->client));

        /*
         * Always force-finish after streaming ends, regardless of how
         * the loop exited (normal completion, parser error, or network
         * error).  This guarantees the parser returns to IDLE so
         * subsequent agent_loop_run() calls can add messages.
         */
        debug_log("[debug] calling llm_parser_force_finish (err=%s)\n",
                 llm_parser_get_error(rt->parser) ? llm_parser_get_error(rt->parser) : "none");
        llm_parser_force_finish(rt->parser);
        debug_log("[debug] after force_finish (err=%s)\n",
                 llm_parser_get_error(rt->parser) ? llm_parser_get_error(rt->parser) : "none");

        /*
         * Check whether the stream ended abnormally
         * (cancellation, mid-stream network error, etc.).
         */
        int stream_error = (stream_client_get_state(rt->client) == CLIENT_STATE_ERROR
                            && !stream_was_cancelled && !llm_runtime_is_cancelled(rt));
        if (stream_was_cancelled || llm_runtime_is_cancelled(rt) || stream_error) {
            debug_log("\n[debug] tool loop iter=%d: stream ended abnormally (cancelled=%d runtime_cancelled=%d stream_error=%d)\n",
                    loop_count, stream_was_cancelled, llm_runtime_is_cancelled(rt), stream_error);

            if (stream_error) {
                /* Genuine mid-stream error (not user cancellation).
                 * Report it so the REPL can show an error message. */
                int ec = stream_client_get_error_code(rt->client);
                const char *emsg = stream_client_get_error_message(rt->client);
                if (ec > 0 && emsg) {
                    set_error(rt, "stream error (code %d): %s", ec, emsg);
                } else {
                    set_error(rt, "stream error: connection lost mid-response");
                }
                if (on_chunk) {
                    on_chunk(rt, LLM_RT_EVENT_ERROR, rt->error_msg,
                             NULL, user_data);
                }
            }

            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
            }
            llm_parser_trim(rt->parser);
            tool_arena_trim();
            return stream_error ? -1 : 0;
        }

        /* ---- Step 6: Check for tool calls in the last assistant message ---- */
        if (!saw_tool_calls) {
            debug_log("\n[debug] tool loop iter=%d: no tool_calls detected in stream, ending turn\n", loop_count);
            break;  /* no tools -> turn complete */
        }

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
            cJSON_GetArraySize(tool_calls_json) == 0) {
            debug_log("\n[debug] tool loop iter=%d: tool_calls_json missing/empty in last message, ending turn\n", loop_count);
            break;
        }

        /* ---- Step 7: Notify tool calls and execute them ---- */
        debug_log("[debug] Step 7: about to execute %d tool call(s)\n",
                 cJSON_GetArraySize(tool_calls_json));
        if (on_chunk) {
            on_chunk(rt, LLM_RT_EVENT_TOOL_CALLS, NULL,
                     tool_calls_json, user_data);
        }

        int executed = execute_tool_calls(rt, tool_calls_json,
                                          on_chunk, user_data);
        debug_log("\n[debug] tool loop iter=%d: executed %d tool(s)\n", loop_count, executed);
        if (executed < 0) {
            debug_log("[debug] execute_tool_calls FAILED (executed=%d), returning -1\n", executed);
            set_error(rt, "tool execution failed");
            return -1;
        }

        /* If cancellation was requested during tool execution, don't loop */
        if (llm_runtime_is_cancelled(rt)) {
            debug_log("\n[debug] tool loop iter=%d: cancelled after tool execution, stopping\n", loop_count);
            if (on_chunk) {
                on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
            }
            llm_parser_trim(rt->parser);
            tool_arena_trim();
            return 0;
        }

        /* Fire DONE per-iteration so the UI can print usage for this step */
        if (on_chunk) {
            on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
        }

        /* ---- Step 8: loop back to send another request ---- */
        debug_log("[debug] tool loop iter=%d: looping back for next API request\n", loop_count);
    }

    /* ---- Step 9: Notify completion ---- */
    debug_log("\n[debug] tool loop: turn complete after %d iteration(s)\n", loop_count);
    if (loop_count >= LLM_RUNTIME_MAX_TOOL_LOOPS) {
        printf("\n\033[1;33m[tool loop limit] %d iterations reached, ending turn. "
               "Continue in next message.\033[0m\n", LLM_RUNTIME_MAX_TOOL_LOOPS);
    }
    debug_log("\n[debug] tool loop: turn complete after %d iteration(s)\n", loop_count);
    if (on_chunk) {
        on_chunk(rt, LLM_RT_EVENT_DONE, NULL, NULL, user_data);
    }

    /* Final trim after the entire turn completes */
    debug_log("[debug] ===== agent_loop_run EXIT (ret=0) =====\n");
    llm_parser_trim(rt->parser);
    tool_arena_trim();

    return 0;
}
