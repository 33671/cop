/*
 * cop_ui.c
 *
 * Terminal UI for cop: REPL, tab completion, streaming output formatting.
 */

#include "cop_ui.h"
#include "tool_functions.h"
#include "isocline/include/isocline.h"
#include "cjson/cJSON.h"
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

/* ============================================================================
 * Tab Completion: /slash commands and @filenames
 * ============================================================================ */
static const char *slash_commands[] = {
    "/model",
    "/set_model ",
    "/sessions",
    "/load ",
    "/delete ",
    "/delete all",
    "/export",
    NULL
};

static void slash_completer(ic_completion_env_t *cenv, const char *prefix) {
    if (!prefix) return;

    /* /slash commands */
    if (prefix[0] == '/') {
        for (int i = 0; slash_commands[i]; i++) {
            if (strncmp(slash_commands[i], prefix, strlen(prefix)) == 0) {
                ic_add_completion(cenv, slash_commands[i]);
            }
        }
        return;
    }

    /* @filename completion */
    if (prefix[0] == '@') {
        const char *name = prefix + 1;
        size_t nlen = strlen(name);

        DIR *dir = opendir(".");
        if (!dir) return;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            if (nlen == 0 || strncmp(entry->d_name, name, nlen) == 0) {
                char buf[512];
                int is_dir = (entry->d_type == DT_DIR);
                if (is_dir) {
                    snprintf(buf, sizeof(buf), "@%s/", entry->d_name);
                } else {
                    snprintf(buf, sizeof(buf), "@%s ", entry->d_name);
                }
                ic_add_completion(cenv, buf);
            }
        }
        closedir(dir);
    }
}

static bool slash_is_word_char(const char *s, long len) {
    (void)len;
    return (*s != ' ' && *s != '\t' && *s != '\n');
}

static void completer_wrapper(ic_completion_env_t *cenv, const char *prefix) {
    ic_complete_word(cenv, prefix, slash_completer, slash_is_word_char);
}

/* ============================================================================
 * Streaming Callback
 * ============================================================================ */

static int  cb_in_reasoning  = 0;
static int  cb_in_responding = 0;

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
            cb_in_reasoning = 1;
        }
        printf("\033[90m%s\033[0m", text);
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
        if (text && strcmp(text, "LLM_WRITING_TOOL_CALL") == 0) {
            if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
            if (cb_in_reasoning)  { printf("\n"); cb_in_reasoning = 0; }
            const char *pv = NULL;
            if (data) {
                cJSON *pj = cJSON_GetObjectItem(data, "preview");
                if (pj && cJSON_IsString(pj)) pv = pj->valuestring;
            }
            if (pv && pv[0]) {
                /* Get actual terminal width */
                int term_w = 80;
                struct winsize ws;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                    term_w = ws.ws_col;
                }
                /* Scroll: show rightmost portion fitting available width */
                int plen = (int)strlen(pv);
                int prefix = 10;  /* "  tool: " */
                int avail = term_w - prefix;
                if (avail < 20) avail = 20;
                int show = plen;
                int start = 0;
                if (show > avail) {
                    start = show - avail;
                    show = avail;
                }
                printf("\r\033[K  \033[33mtool:\033[0m \033[1;33m%.*s\033[0m",
                       show, pv + start);
            } else {
                printf("\r\033[K\033[90m[writing tool call...]\033[0m");
            }
            fflush(stdout);
        }
        break;

    case LLM_RT_EVENT_TOOL_CALLS:
        printf("\r\033[K");  /* erase in-progress preview */
        if (cb_in_reasoning)  { printf("\n"); cb_in_reasoning = 0; }
        if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
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

            const char *first_nl = strchr(preview, '\n');
            if (first_nl && *(first_nl + 1)) {
                printf("  \033[32m-> %s\033[0m \033[90m%.*s\033[0m\n",
                       name_str, (int)(first_nl - preview), preview);
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
            /* Context window usage */
            {
                const char *model_id = llm_runtime_get_model(rt);
                const model_entry_t *entry = models_config_find(g_models, model_id);
                if (entry && entry->context_window > 0) {
                    int ctx = entry->context_window;
                    int used = t && cJSON_IsNumber(t) ? t->valueint : 0;
                    double pct = (ctx > 0) ? (used * 100.0 / ctx) : 0;
                    char ctxbuf[16];
                    printf("  \033[90mctx: %.1f%%/%s\033[0m",
                           pct, fmt_tokens(ctx, ctxbuf, sizeof(ctxbuf)));
                }
            }
            printf("\033[0m\n");
        }
        break;

    case LLM_RT_EVENT_DONE:
        printf("\r\033[K");  /* erase any leftover preview */
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
 * Command Handlers
 * ============================================================================ */

static void cmd_sessions(llm_runtime_t *rt) {
    (void)rt;
    cJSON *list = history_db_list_sessions(g_db, g_cwd);
    if (!list) return;

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
               marker, sid_val,
               mc ? mc->valueint : 0,
               cat ? cat->valuestring : "?",
               lum && cJSON_IsString(lum) ? lum->valuestring
                                           : "\033[90m(empty)\033[0m");
    }
    if (n == 0) printf("  \033[90m(no sessions for %s)\033[0m\n", g_cwd);
    printf("  \033[90m(* = current)\033[0m\n");
    cJSON_Delete(list);
}

static void cmd_load(llm_runtime_t *rt, const char *arg) {
    int64_t sid = strtoll(arg, NULL, 10);
    if (sid <= 0) return;

    cJSON *msgs = history_db_load_session(g_db, sid);
    if (!msgs || cJSON_GetArraySize(msgs) == 0) {
        printf("Session %lld not found or empty.\n", (long long)sid);
        cJSON_Delete(msgs);
        return;
    }

    llm_runtime_reset(rt);
    int n = cJSON_GetArraySize(msgs);

    /* Print last 50 messages */
    int print_start = (n > 50) ? n - 50 : 0;
    printf("\n  \033[90m── History (messages %d–%d of %d) ──\033[0m\n",
           print_start + 1, n, n);

    for (int i = print_start; i < n; i++) {
        cJSON *msg    = cJSON_GetArrayItem(msgs, i);
        cJSON *r      = cJSON_GetObjectItem(msg, "role");
        cJSON *c      = cJSON_GetObjectItem(msg, "content");
        cJSON *rc     = cJSON_GetObjectItem(msg, "reasoning_content");
        cJSON *tc_id  = cJSON_GetObjectItem(msg, "tool_call_id");
        cJSON *tc     = cJSON_GetObjectItem(msg, "tool_calls");

        const char *role    = (r && cJSON_IsString(r)) ? r->valuestring : "?";
        const char *raw     = (c && cJSON_IsString(c)) ? c->valuestring : NULL;

        if (strcmp(role, "user") == 0) {
            /* User: keep truncation (200 chars / 3 lines) */
            if (raw && raw[0]) {
                size_t rlen = strlen(raw);
                int lines = 0;
                size_t e = 0;
                for (size_t j = 0; j < rlen && j < 200 && lines < 3; j++) {
                    if (raw[j] == '\n') lines++;
                    e++;
                }
                int truncated = (e < rlen);
                printf("  \033[1;32m[%d] user:\033[0m %.*s%s\n",
                       i + 1, (int)e, raw, truncated ? "..." : "");
            } else {
                printf("  \033[1;32m[%d] user:\033[0m\n", i + 1);
            }
        } else if (strcmp(role, "assistant") == 0) {
            /* Reasoning: light gray, NO truncation, no label */
            if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
                printf("\033[90m%s\033[0m\n", rc->valuestring);
            }
            /* Content: bold blue, NO truncation, no label */
            if (raw && raw[0]) {
                printf("\033[1;34m%s\033[0m\n", raw);
            }
            /* Tool calls: same format as runtime, complete display */
            if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0) {
                int tcn = cJSON_GetArraySize(tc);
                printf("\n");
                for (int j = 0; j < tcn; j++) {
                    cJSON *t  = cJSON_GetArrayItem(tc, j);
                    cJSON *fn = cJSON_GetObjectItem(t, "function");
                    cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                    cJSON *ar = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                    const char *nstr = (nm && cJSON_IsString(nm)) ? nm->valuestring : "?";
                    const char *astr = (ar && cJSON_IsString(ar)) ? ar->valuestring : "";
                    printf("  \033[33mtool:\033[0m \033[1;33m%s\033[0m", nstr);
                    if (astr && astr[0]) {
                        size_t alen = strlen(astr);
                        if (alen > 80) {
                            printf(" \033[90m%.80s...\033[0m", astr);
                        } else {
                            printf(" \033[90m%s\033[0m", astr);
                        }
                    }
                    printf("\n");
                }
            }
        } else if (strcmp(role, "tool") == 0) {
            /* Tool result: same format as runtime, truncate to 3 lines */
            if (raw && raw[0]) {
                const char *first_nl = strchr(raw, '\n');
                if (first_nl && *(first_nl + 1)) {
                    int first_len = (int)(first_nl - raw);
                    printf("  \033[32m->\033[0m \033[90m%.*s\033[0m\n",
                           first_len, raw);
                    const char *rest = first_nl + 1;
                    int line_count = 0;
                    while (*rest && line_count < 2) {
                        const char *next = strchr(rest, '\n');
                        if (next) {
                            printf("     \033[90m%.*s\033[0m\n",
                                   (int)(next - rest), rest);
                            rest = next + 1;
                        } else {
                            printf("     \033[90m%s\033[0m\n", rest);
                            break;
                        }
                        line_count++;
                    }
                    if (*rest) {
                        printf("     \033[90m...\033[0m\n");
                    }
                } else {
                    printf("  \033[32m->\033[0m \033[90m%s\033[0m\n", raw);
                }
            } else {
                printf("  \033[32m->\033[0m\n");
            }
        } else {
            /* Unknown role — truncated */
            int show = raw ? (int)strlen(raw) : 0;
            if (show > 120) show = 120;
            printf("  \033[90m[%d] %s:\033[0m %.*s%s\n",
                   i + 1, role, show, raw ? raw : "",
                   raw && (int)strlen(raw) > 120 ? "..." : "");
        }
    }
    printf("  \033[90m── end of history ──\033[0m\n\n");

    for (int i = 0; i < n; i++) {
        llm_runtime_add_message(rt, cJSON_GetArrayItem(msgs, i));
    }
    g_session_id = sid;
    g_saved_count = history_db_get_saved_count(g_db, sid);
    if (g_saved_count < 0) g_saved_count = 0;
    printf("Loaded session %lld with %d messages.\n", (long long)sid, n);
    cJSON_Delete(msgs);
}

static void cmd_delete(llm_runtime_t *rt, const char *arg) {
    if (!arg || !arg[0]) {
        printf("Usage: /delete <id> | /delete all\n");
        return;
    }
    if (strcmp(arg, "all") == 0) {
        int n = history_db_delete_sessions_by_cwd(g_db, g_cwd);
        if (n >= 0) {
            g_session_id = -1;
            g_saved_count = 0;
            llm_runtime_reset(rt);
            printf("Deleted %d session(s) in %s\n", n, g_cwd);
        } else {
            printf("Failed to delete sessions.\n");
        }
        return;
    }

    int64_t did = strtoll(arg, NULL, 10);
    if (did <= 0) return;

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

static void cmd_export(llm_runtime_t *rt) {
    (void)rt;
    const cJSON *history = llm_runtime_get_history(rt);
    if (!history) {
        printf("No conversation to export.\n");
        return;
    }
    const cJSON *msgs = cJSON_GetObjectItem(history, "messages");
    int count = msgs ? cJSON_GetArraySize(msgs) : 0;
    if (count == 0) {
        printf("No messages to export.\n");
        return;
    }

    /* Build timestamp filename: cop_export_20260508_143022.json */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char fname[128];
    snprintf(fname, sizeof(fname),
             "cop_export_%04d%02d%02d_%02d%02d%02d.json",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    char *json_str = cJSON_Print(history);
    if (!json_str) {
        printf("Error: cJSON_Print failed.\n");
        return;
    }

    FILE *f = fopen(fname, "w");
    if (!f) {
        printf("Error: cannot write to %s\n", fname);
        free(json_str);
        return;
    }
    fprintf(f, "%s\n", json_str);
    fclose(f);
    free(json_str);

    printf("Exported %d messages → %s\n", count, fname);
}

static void cmd_model(llm_runtime_t *rt) {
    if (!g_models) {
        printf("No models loaded (check ~/.cop/models.json).\n");
        return;
    }
    printf("\n");
    const char *current = llm_runtime_get_model(rt);
    const char *last_prov = NULL;
    for (int i = 0; g_models[i]; i++) {
        model_entry_t *m = g_models[i];
        if (!last_prov || strcmp(m->provider, last_prov) != 0) {
            printf("  \033[1;36m%s\033[0m  (%s)\n", m->provider, m->base_url);
            last_prov = m->provider;
        }
        const char *marker = (current && strcmp(m->model_id, current) == 0)
                             ? "\033[32m*\033[0m" : " ";
        printf("   %s \033[33m%s\033[0m", marker, m->model_id);
        if (m->context_window > 0) {
            printf("  \033[90m%dk ctx\033[0m", m->context_window / 1000);
        }
        printf("\n");
    }
    printf("\n  \033[90m* = current\033[0m\n");
}

static void cmd_set_model(llm_runtime_t *rt, const char *model_id) {
    const model_entry_t *entry = models_config_find(g_models, model_id);
    if (!entry) {
        printf("Model '%s' not found. Use /model to list.\n", model_id);
        return;
    }

    char endpoint[512];
    const char *base = entry->base_url;
    size_t blen = strlen(base);
    if (blen > 0 && base[blen - 1] == '/') {
        snprintf(endpoint, sizeof(endpoint),
                 "%.*s/chat/completions", (int)(blen - 1), base);
    } else {
        snprintf(endpoint, sizeof(endpoint),
                 "%s/chat/completions", base);
    }
    llm_runtime_set_model(rt, entry->model_id, entry->api_key, endpoint);
    printf("Switched to \033[1;33m%s\033[0m (\033[36m%s\033[0m)\n",
           entry->model_id, entry->provider);
}

/* ============================================================================
 * REPL
 * ============================================================================ */

coroutine void cop_ui_repl(llm_runtime_t *rt) {
    while (1) {
        yield();

        char *line = ic_readline("[green][b]User[/] ");
        if (!line) break;

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) { free(line); continue; }

        /* ── Commands ── */
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            free(line);
            printf("Goodbye!\n");
            break;
        }
        if (strcmp(line, "/sessions") == 0) {
            cmd_sessions(rt);
            free(line); continue;
        }
        if (strncmp(line, "/load ", 6) == 0) {
            cmd_load(rt, line + 6);
            free(line); continue;
        }
        if (strncmp(line, "/delete ", 8) == 0) {
            cmd_delete(rt, line + 8);
            free(line); continue;
        }
        if (strcmp(line, "/model") == 0) {
            cmd_model(rt);
            free(line); continue;
        }
        if (strcmp(line, "/export") == 0) {
            cmd_export(rt);
            free(line); continue;
        }
        if (strncmp(line, "/set_model ", 11) == 0) {
            cmd_set_model(rt, line + 11);
            free(line); continue;
        }

        /* ── Send message ── */
        fflush(stdout);
        int ret = llm_runtime_send(rt, line, on_runtime_event, NULL);
        if (llm_runtime_is_cancelled(rt)) {
            printf("\n[Cancelled]\n");
        }
        if (ret != 0) {
            const char *err = llm_runtime_get_error(rt);
            printf("\n\033[31mError: %s\033[0m\n", err ? err : "send failed");
        }
        save_history_step(rt);

        free(line);
        msleep(now() + 100);
    }

    printf("\nGoodbye!\n");
    _exit(0);
}

/* ============================================================================
 * UI Init
 * ============================================================================ */

void cop_ui_init(void) {
    ic_enable_multiline(true);

    /* Expand ~ for isocline history path */
    char hist_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(hist_path, sizeof(hist_path), "%s/.cop/.history", home);
    ic_set_history(hist_path, 100);

    ic_set_default_completer(completer_wrapper, NULL);
}

void cop_ui_banner(const char *model, const char *endpoint,
                    const char *log_file, const char *cwd) {
    printf("Model:    %s\n", model);
    printf("Endpoint: %s\n", endpoint);
    printf("Log:      %s\n", log_file);
    printf("CWD:      %s\n", cwd);
    if (g_rt && llm_runtime_is_yolo(g_rt))
        printf("Mode:     YOLO — auto-approving all tool calls\n");
    printf("Input:    Enter=submit, Shift+Enter/Ctrl+J=newline\n");
    printf("Commands: /model, /set_model <id>, /sessions, /load, /delete, /export\n");
}

void cop_ui_sigint(int sig, siginfo_t *info, void *uap) {
    (void)sig; (void)info; (void)uap;
    if (g_rt && !llm_runtime_is_cancelled(g_rt)) {
        fprintf(stderr, "\n[debug] SIGINT: cancelling runtime\n");
        llm_runtime_cancel(g_rt);
        write(STDOUT_FILENO, "\n[Cancelling...]\n", 17);
    } else {
        fprintf(stderr, "\n[debug] SIGINT: already cancelled, exiting\n");
        write(STDOUT_FILENO, "\n[Exiting...]\n", 14);
        _exit(0);
    }
}
