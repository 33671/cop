/*
 * llm_runtime_test.c
 *
 * Test program demonstrating the high-level LLM runtime.
 * Uses isocline for rich terminal input (multiline, history, bbcode colors).
 *
 * Features:
 *   - Multi-turn conversation (history maintained automatically)
 *   - Tool registration and automatic execution loop
 *   - Streaming callbacks for content/reasoning/tool_calls
 *   - Multiline input: Enter submits, Shift+Enter / Ctrl+J inserts newline
 *   - Colored terminal output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>
#include "utils.h"
#include "llm_runtime.h"
#include "tool_functions.h"
#include "history_db.h"
#include "models_config.h"
#include "libmill/libmill.h"
#include "isocline/include/isocline.h"

/* ============================================================================
 * Global State
 * ============================================================================ */
static llm_runtime_t    *g_rt = NULL;
static history_db_t     *g_db = NULL;
static int64_t           g_session_id = -1;
static int               g_saved_count = 0;
static char              g_cwd[4096];
static model_entry_t   **g_models = NULL;   /* loaded from ~/.agent/models.json */

void sigint_handler(int sig) {
    (void)sig;
    if (g_rt && !llm_runtime_is_cancelled(g_rt)) {
        /* First Ctrl+C: cancel the current streaming turn */
        llm_runtime_cancel(g_rt);
        write(STDOUT_FILENO, "\n[Cancelling...]\n", 17);
    } else {
        /* Second Ctrl+C: exit */
        write(STDOUT_FILENO, "\n[Exiting...]\n", 14);
        _exit(0);
    }
}

/* (Tool functions moved to tool_functions.c / tool_functions.h) */

/* ============================================================================
 * Streaming Callback
 * ============================================================================ */

/* Visual state tracked across events */
static int  cb_in_reasoning  = 0;
static int  cb_in_responding = 0;

/* Format token count: 1234 -> "1.2k", 567 -> "567" */
static const char *fmt_tokens(int n, char *buf, size_t bufsz) {
    if (n >= 1000) {
        snprintf(buf, bufsz, "%.1fk", n / 1000.0);
    } else {
        snprintf(buf, bufsz, "%d", n);
    }
    return buf;
}

static void on_runtime_event(llm_runtime_t *rt,
                              llm_runtime_event_t event,
                              const char *text,
                              const cJSON *data,
                              void *user_data) {
    (void)rt;
    (void)user_data;

    switch (event) {

    case LLM_RT_EVENT_REASONING:
        if (!cb_in_reasoning) {
            if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
            printf("\033[90m[reasoning]\033[0m\n");
            cb_in_reasoning = 1;
        }
        printf("\033[2;3m%s\033[0m", text);
        fflush(stdout);
        break;

    case LLM_RT_EVENT_CONTENT:
        if (!cb_in_responding) {
            if (cb_in_reasoning) { printf("\n"); cb_in_reasoning = 0; }
            cb_in_responding = 1;
        }
        printf("\033[1;34m%s\033[0m", text);
        fflush(stdout);
        break;

    case LLM_RT_EVENT_STATUS_CHANGE:
        if (cb_in_reasoning && text && strcmp(text, "REASONING") != 0) {
            printf("\n");
            cb_in_reasoning = 0;
        }
        if (cb_in_responding && text && strcmp(text, "WRITING_TOOL_CALL") == 0) {
            printf("\n");
            cb_in_responding = 0;
        }
        break;

    case LLM_RT_EVENT_TOOL_CALLS:
        printf("\n");
        if (data && cJSON_IsArray(data)) {
            int n = cJSON_GetArraySize(data);
            for (int i = 0; i < n; i++) {
                cJSON *tc   = cJSON_GetArrayItem(data, i);
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                cJSON *name = func ? cJSON_GetObjectItem(func, "name") : NULL;
                cJSON *args = func ? cJSON_GetObjectItem(func, "arguments") : NULL;

                const char *n = (name && cJSON_IsString(name)) ? name->valuestring : "?";
                const char *a = (args && cJSON_IsString(args)) ? args->valuestring : "";

                printf("  \033[33mtool:\033[0m \033[1;33m%s\033[0m", n);
                if (a && a[0]) {
                    size_t alen = strlen(a);
                    if (alen > 80) {
                        printf(" \033[90m%.80s...\033[0m", a);
                    } else {
                        printf(" \033[90m%s\033[0m", a);
                    }
                }
                printf("\n");
            }
        }
        break;

    case LLM_RT_EVENT_TOOL_RESULT:
        if (data) {
            cJSON *nm = cJSON_GetObjectItem(data, "name");
            cJSON *pv = cJSON_GetObjectItem(data, "preview");
            const char *name_str = (nm && cJSON_IsString(nm)) ? nm->valuestring : "?";
            const char *preview  = (pv && cJSON_IsString(pv)) ? pv->valuestring : "";

            /* Show first line of preview inline, rest indented */
            const char *first_nl = strchr(preview, '\n');
            if (first_nl && *(first_nl + 1)) {
                printf("  \033[32m-> %s\033[0m \033[90m%.*s\033[0m\n",
                       name_str, (int)(first_nl - preview), preview);
                /* Print remaining lines indented */
                const char *rest = first_nl + 1;
                while (*rest) {
                    const char *next = strchr(rest, '\n');
                    if (next) {
                        printf("     \033[90m%.*s\033[0m\n", (int)(next - rest), rest);
                        rest = next + 1;
                    } else {
                        printf("     \033[90m%s\033[0m\n", rest);
                        break;
                    }
                }
            } else {
                printf("  \033[32m-> %s\033[0m \033[90m%s\033[0m\n", name_str, preview);
            }
        }
        break;

    case LLM_RT_EVENT_USAGE:
        if (data) {
            cJSON *p = cJSON_GetObjectItem(data, "prompt_tokens");
            cJSON *c = cJSON_GetObjectItem(data, "completion_tokens");
            cJSON *h = cJSON_GetObjectItem(data, "cached_tokens");
            cJSON *t = cJSON_GetObjectItem(data, "total_tokens");

            char pi[16], co[16], ca[16], to[16];
            printf("\n\033[90min: %s  out: %s",
                   fmt_tokens(p && cJSON_IsNumber(p) ? p->valueint : 0, pi, sizeof(pi)),
                   fmt_tokens(c && cJSON_IsNumber(c) ? c->valueint : 0, co, sizeof(co)));
            if (h && cJSON_IsNumber(h) && h->valueint > 0) {
                printf("  cached: %s",
                       fmt_tokens(h->valueint, ca, sizeof(ca)));
            }
            if (t && cJSON_IsNumber(t)) {
                printf("  total: %s",
                       fmt_tokens(t->valueint, to, sizeof(to)));
            }
            printf("\033[0m\n");
        }
        break;

    case LLM_RT_EVENT_DONE:
        if (cb_in_reasoning)  { printf("\n"); cb_in_reasoning = 0; }
        if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
        break;

    case LLM_RT_EVENT_ERROR:
        cb_in_reasoning  = 0;
        cb_in_responding = 0;
        printf("\n\033[1;31mError: %s\033[0m\n", text ? text : "unknown");
        break;
    }
}

/* ============================================================================
 * History Persistence Helper
 * ============================================================================ */
static void save_history_step(llm_runtime_t *rt) {
    if (!g_db) return;

    const cJSON *history = llm_runtime_get_history(rt);
    if (!history) return;

    const cJSON *msgs = cJSON_GetObjectItem(history, "messages");
    if (!msgs || cJSON_GetArraySize(msgs) == 0) return;

    /* Lazy session creation: only create when there are messages to save */
    if (g_session_id < 0) {
        g_session_id = history_db_new_session(g_db, g_cwd);
        if (g_session_id < 0) {
            fprintf(stderr, "\n[history_db] failed to create session\n");
            return;
        }
        g_saved_count = 0;
    }

    if (history_db_save_step(g_db, g_session_id, &g_saved_count, msgs) != 0) {
        fprintf(stderr, "\n[history_db] failed to save messages\n");
    }
}

/* ============================================================================
 * Main Coroutine: Interactive REPL (isocline multiline input)
 * ============================================================================ */
coroutine void chat_loop(llm_runtime_t *rt) {
    while (1) {
        /* Let other coroutines run before blocking on input */
        yield();

        /*
         * Read input with isocline.
         *
         * Multiline mode is enabled: Enter submits, Shift+Enter / Ctrl+J
         * inserts a literal newline.
         *
         * The prompt uses BBCode markup for color (isocline's native format):
         *   $bold$green  You: $reset$  -> bold green "You: "
         * isocline handles BBCode correctly in prompt width calculations.
         */
        char *line = ic_readline("[green][b]User[/] ");
        if (!line) {
            /* EOF (Ctrl+D) or Ctrl+C (in empty line) */
            break;
        }

        /* Trim trailing newlines that isocline might leave */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            free(line);
            printf("Goodbye!\n");
            break;
        }
        if (strcmp(line, "/sessions") == 0) {
            cJSON *list = history_db_list_sessions(g_db, g_cwd);
            if (list) {
                int n = cJSON_GetArraySize(list);
                printf("\n  \033[1m%-4s  %-6s  %-19s  %s\033[0m\n",
                       "ID", "Msgs", "Created", "Last User Message");
                for (int i = 0; i < n; i++) {
                    cJSON *s   = cJSON_GetArrayItem(list, i);
                    cJSON *id  = cJSON_GetObjectItem(s, "id");
                    cJSON *mc  = cJSON_GetObjectItem(s, "msg_count");
                    cJSON *cat = cJSON_GetObjectItem(s, "created_at");
                    cJSON *lum = cJSON_GetObjectItem(s, "last_user_msg");

                    int sid_val = id ? id->valueint : 0;
                    const char *marker = (sid_val == g_session_id) ? "\033[32m*\033[0m " : "  ";

                    printf("%s%-4d  %-6d  %-19s  %s\n",
                           marker,
                           sid_val,
                           mc ? mc->valueint : 0,
                           cat ? cat->valuestring : "?",
                           lum && cJSON_IsString(lum) ? lum->valuestring
                                                       : "\033[90m(empty)\033[0m");
                }
                if (n == 0) printf("  \033[90m(no sessions for %s)\033[0m\n", g_cwd);
                printf("  \033[90m(* = current)\033[0m\n");
                cJSON_Delete(list);
            }
            free(line);
            continue;
        }
        if (strncmp(line, "/load ", 6) == 0) {
            int64_t sid = strtoll(line + 6, NULL, 10);
            if (sid > 0) {
                cJSON *msgs = history_db_load_session(g_db, sid);
                if (msgs && cJSON_GetArraySize(msgs) > 0) {
                    llm_runtime_reset(rt);
                    int n = cJSON_GetArraySize(msgs);

                    /* Print the last 50 messages */
                    int print_start = (n > 50) ? n - 50 : 0;
                    printf("\n  ── History (messages %d-%d of %d) ──\n",
                           print_start + 1, n, n);
                    for (int i = print_start; i < n; i++) {
                        cJSON *msg    = cJSON_GetArrayItem(msgs, i);
                        cJSON *r      = cJSON_GetObjectItem(msg, "role");
                        cJSON *c      = cJSON_GetObjectItem(msg, "content");
                        cJSON *rc     = cJSON_GetObjectItem(msg, "reasoning_content");
                        cJSON *tc_id  = cJSON_GetObjectItem(msg, "tool_call_id");
                        cJSON *tc     = cJSON_GetObjectItem(msg, "tool_calls");

                        const char *role = (r && cJSON_IsString(r)) ? r->valuestring : "?";
                        const char *content = (c && cJSON_IsString(c)) ? c->valuestring : NULL;

                        if (strcmp(role, "user") == 0) {
                            printf("  \033[1;32m[%d] user:\033[0m %s\n",
                                   i + 1, content ? content : "");
                        } else if (strcmp(role, "assistant") == 0) {
                            printf("  \033[1;34m[%d] assistant:\033[0m %s\n",
                                   i + 1, content ? content : "(tool_calls only)");
                            if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
                                printf("  \033[2;3m       reasoning: %s\033[0m\n",
                                       rc->valuestring);
                            }
                            if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
                                printf("  \033[33m       tool_calls:\033[0m ");
                                int tcn = cJSON_GetArraySize(tc);
                                for (int j = 0; j < tcn; j++) {
                                    cJSON *t  = cJSON_GetArrayItem(tc, j);
                                    cJSON *fn = cJSON_GetObjectItem(t, "function");
                                    cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                                    printf("%s%s", j > 0 ? ", " : "",
                                           (nm && cJSON_IsString(nm)) ? nm->valuestring : "?");
                                }
                                printf("\n");
                            }
                        } else if (strcmp(role, "tool") == 0) {
                            const char *tid = (tc_id && cJSON_IsString(tc_id))
                                              ? tc_id->valuestring : "?";
                            printf("  \033[1;33m[%d] tool(%s):\033[0m %s\n",
                                   i + 1, tid, content ? content : "");
                        } else {
                            printf("  \033[90m[%d] %s:\033[0m %s\n",
                                   i + 1, role, content ? content : "");
                        }
                    }
                    printf("  ── end of history ──\n\n");

                    for (int i = 0; i < n; i++) {
                        cJSON *msg = cJSON_GetArrayItem(msgs, i);
                        llm_runtime_add_message(rt, msg);
                    }
                    g_session_id = sid;
                    g_saved_count = history_db_get_saved_count(g_db, sid);
                    if (g_saved_count < 0) g_saved_count = 0;
                    printf("Loaded session %lld with %d messages.\n",
                           (long long)sid, n);
                } else {
                    printf("Session %lld not found or empty.\n", (long long)sid);
                }
                cJSON_Delete(msgs);
            }
            free(line);
            continue;
        }
        if (strncmp(line, "/delete ", 8) == 0) {
            int64_t did = strtoll(line + 8, NULL, 10);
            if (did > 0) {
                if (history_db_delete_session(g_db, did) == 0) {
                    printf("Deleted session %lld.\n", (long long)did);
                    if (did == g_session_id) {
                        g_session_id = -1;
                        g_saved_count = 0;
                        llm_runtime_reset(rt);
                        printf("(current session was deleted, starting fresh)\n");
                    }
                } else {
                    printf("Failed to delete session %lld.\n", (long long)did);
                }
            }
            free(line);
            continue;
        }
        if (strcmp(line, "/model") == 0) {
            if (!g_models) {
                printf("No models loaded (check ~/.agent/models.json).\n");
            } else {
                printf("\n");
                const char *current = llm_runtime_get_model(rt);
                const char *last_prov = NULL;
                for (int i = 0; g_models[i]; i++) {
                    model_entry_t *m = g_models[i];
                    if (!last_prov || strcmp(m->provider, last_prov) != 0) {
                        printf("  \033[1;36m%s\033[0m  (%s)\n",
                               m->provider, m->base_url);
                        last_prov = m->provider;
                    }
                    const char *marker = (current && strcmp(m->model_id, current) == 0)
                                         ? "\033[32m*\033[0m" : " ";
                    printf("   %s \033[33m%s\033[0m",
                           marker, m->model_id);
                    if (m->context_window > 0) {
                        printf("  \033[90m%dk ctx\033[0m",
                               m->context_window / 1000);
                    }
                    printf("\n");
                }
                printf("\n  \033[90m* = current\033[0m\n");
            }
            free(line);
            continue;
        }
        if (strncmp(line, "/set_model ", 11) == 0) {
            const char *model_id = line + 11;
            const model_entry_t *entry = models_config_find(g_models, model_id);
            if (!entry) {
                printf("Model '%s' not found. Use /model to list.\n", model_id);
            } else {
                char endpoint[512];
                const char *base = entry->base_url;
                size_t blen = strlen(base);
                /* Build endpoint, stripping any trailing slash from base_url */
                if (blen > 0 && base[blen - 1] == '/') {
                    snprintf(endpoint, sizeof(endpoint),
                             "%.*s/chat/completions", (int)(blen - 1), base);
                } else {
                    snprintf(endpoint, sizeof(endpoint),
                             "%s/chat/completions", base);
                }
                llm_runtime_set_model(rt, entry->model_id,
                                      entry->api_key, endpoint);
                printf("Switched to \033[1;33m%s\033[0m (\033[36m%s\033[0m)\n",
                       entry->model_id, entry->provider);
            }
            free(line);
            continue;
        }

        /* Send message and stream the response */
        // ic_print("[green][b]Assistant:[/] ");
        fflush(stdout);

        int ret = llm_runtime_send(rt, line, on_runtime_event, NULL);
        if (ret != 0) {
            if (llm_runtime_is_cancelled(rt)) {
                printf("\n[Cancelled]\n");
                /* Save partial progress (user msg + any tool results) */
                save_history_step(rt);
            } else {
                const char *err = llm_runtime_get_error(rt);
                printf("\n\033[31mError: %s\033[0m\n", err ? err : "send failed");
            }
        } else {
            /* Save complete turn to history DB */
            save_history_step(rt);
        }

        free(line);

        /* Brief pause so the user can read output before next prompt */
        msleep(now() + 100);
    }

    printf("\nGoodbye!\n");
    _exit(0);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    /* Load model config from ~/.agent/models.json */
    g_models = models_config_load();
    if (!g_models || !g_models[0]) {
        fprintf(stderr, "Error: no models found in ~/.agent/models.json\n"
                "Create it with format:\n"
                "{\"providers\":{\"name\":{\"baseUrl\":\"...\","
                "\"apiKey\":\"...\",\"models\":[{\"id\":\"...\"}]}}}\n");
        return 1;
    }

    /* Default: first model in config */
    const char *model        = g_models[0]->model_id;
    const char *api_key      = g_models[0]->api_key;
    char        api_endpoint[512];
    snprintf(api_endpoint, sizeof(api_endpoint),
             "%s/chat/completions", g_models[0]->base_url);
    const char *log_file     = "llm_runtime.log";

    /* Parse CLI args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_file = argv[++i];
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Configure isocline */
    ic_enable_multiline(true);         /* Enter=submit, Shift+Enter=newline */
    ic_set_history(".history", 100);   /* persistent history */

    /* Create runtime */
    llm_runtime_t *rt = llm_runtime_new(api_key, model, api_endpoint, log_file);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        curl_global_cleanup();
        return 1;
    }

    /* Capture working directory for session scope */
    if (!getcwd(g_cwd, sizeof(g_cwd))) {
        g_cwd[0] = '\0';
    }

    /* Open history database (session created lazily on first message) */
    history_db_open(&g_db);


    printf("Model:    %-46s\n", model);
    printf("Endpoint: %-44s\n", api_endpoint);
    printf("Log:      %-44s\n", log_file);
    printf("CWD:      %-46s\n", g_cwd);
    printf("Input:    Enter=submit, Shift+Enter/Ctrl+J=newline\n");
    printf("Commands: /model, /set_model <id>, /sessions, /load, /delete\n");

    /* Register tools and schemas */
    llm_runtime_register_tool(rt, "sleep", tool_sleep);
    llm_runtime_register_tool(rt, "shell", tool_shell);
    llm_runtime_register_tool(rt, "read", tool_read);
    llm_runtime_register_tool(rt, "write", tool_write);
    llm_runtime_register_tool(rt, "edit", tool_edit);

    cJSON *schemas = tool_functions_create_schema();
    llm_runtime_set_tool_schema(rt, schemas);
    cJSON_Delete(schemas);

    /* Set global for signal handler access */
    g_rt = rt;

    /* Launch the REPL coroutine */
    go(chat_loop(rt));

    /* Keep the libmill scheduler alive forever */
    while (1) {
        msleep(now() + 100);
    }

    /* Not reached */
    llm_runtime_free(rt);
    curl_global_cleanup();
    return 0;
}
