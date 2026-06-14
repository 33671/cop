/*
 * cop_ui.c
 *
 * Terminal UI for cop: REPL, tab completion, streaming output formatting.
 */

#include "cop_ui.h"
#include "tool_functions.h"
#include "scroll_viewport.h"
#include "isocline/include/isocline.h"
#include "cjson/cJSON.h"
#include "stream_md_renderer.h"
#include "debug.h"
#include <sys/ioctl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

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
    "/resume",
    NULL
};

/* Completion callback for /slash commands only. */
static void slash_completer(ic_completion_env_t *cenv, const char *prefix) {
    if (!prefix) return;
    for (int i = 0; slash_commands[i]; i++) {
        if (strncmp(slash_commands[i], prefix, strlen(prefix)) == 0) {
            ic_add_completion(cenv, slash_commands[i]);
        }
    }
}

static bool slash_is_word_char(const char *s, long len) {
    (void)len;
    return (*s != ' ' && *s != '\t' && *s != '\n');
}

static void completer_wrapper(ic_completion_env_t *cenv, const char *prefix) {
    if (prefix && prefix[0] == '/') {
        /* /slash commands */
        ic_complete_word(cenv, prefix, slash_completer, slash_is_word_char);
    } else {
        /* Built-in filename completion for everything else */
        ic_complete_filename(cenv, prefix ? prefix : "", '/', NULL, NULL);
    }
}

/* ============================================================================
 * Streaming Callback
 * ============================================================================ */

static int  cb_in_reasoning  = 0;
static int  cb_in_responding = 0;
static md_display_t g_content_display;  /* markdown renderer for content */
static int g_content_display_inited = 0;
static scroll_viewport_t *g_reasoning_vp = NULL;  /* viewport for reasoning output */

/* Finish the reasoning viewport: free resources.
 * scroll_viewport_finish() already ensures cursor is on a fresh line. */
static void reasoning_viewport_finish(void) {
    if (g_reasoning_vp) {
        size_t len = 0;
        char *buf = scroll_viewport_finish(g_reasoning_vp, &len);
        free(buf);
        g_reasoning_vp = NULL;
    }
}

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
    cop_context_t *ctx = (cop_context_t *)user_data;

    switch (event) {

    case LLM_RT_EVENT_REASONING:
        if (!cb_in_reasoning) {
            if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
            cb_in_reasoning = 1;
            g_reasoning_vp = scroll_viewport_new_with_ratio(0.3f);
            scroll_viewport_set_style(g_reasoning_vp, "\033[90m", "\033[0m");
        }
        if (g_reasoning_vp && text) {
            scroll_viewport_feed(text, (int)strlen(text), (void *)g_reasoning_vp);
        }
        break;

    case LLM_RT_EVENT_CONTENT:
        if (!cb_in_responding) {
            if (cb_in_reasoning) {
                reasoning_viewport_finish();
                cb_in_reasoning = 0;
            }
            cb_in_responding = 1;
            /* Start a fresh markdown renderer for this response */
            if (!g_content_display_inited) {
                md_display_init(&g_content_display, md_get_terminal_width());
                g_content_display_inited = 1;
            } else {
                md_display_reset(&g_content_display);
            }
        }
        md_display_feed(&g_content_display, text, (int)strlen(text));
        break;

    case LLM_RT_EVENT_STATUS_CHANGE:
        if (text && strcmp(text, "LLM_WRITING_TOOL_CALL") == 0) {
            if (cb_in_responding) { cb_in_responding = 0; }
            if (cb_in_reasoning)  { reasoning_viewport_finish(); cb_in_reasoning = 0; }
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
        fflush(stdout);
        if (cb_in_reasoning)  { reasoning_viewport_finish(); cb_in_reasoning = 0; }
        if (cb_in_responding) { cb_in_responding = 0; }
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

            /* Print tool name */
            printf("  \033[32m-> %s\033[0m\n", name_str);

            if (preview[0]) {
                /* Saved-to-file output → show full preview.
                 * Otherwise (short output already streamed via viewport) → show 2 lines max. */
                int is_saved_file = (strncmp(preview, "Output saved to:", 16) == 0);
                int max_lines = is_saved_file ? 999 : 2;
                int line_no = 0;

                const char *p = preview;
                while (*p && line_no < max_lines) {
                    const char *nl = strchr(p, '\n');
                    if (nl) {
                        printf("     \033[90m%.*s\033[0m\n", (int)(nl - p), p);
                        p = nl + 1;
                        line_no++;
                    } else {
                        printf("     \033[90m%s\033[0m\n", p);
                        break;
                    }
                }
            }
        }
        break;

    case LLM_RT_EVENT_DONE:
        /* Reset markdown renderer for next turn */
        if (g_content_display_inited)
            md_display_reset(&g_content_display);
        /* Print usage stats directly from rt */
        if (llm_runtime_usage_seen(rt)) {
            int p = llm_runtime_usage_prompt(rt);
            int c = llm_runtime_usage_completion(rt);
            int h = llm_runtime_usage_cached(rt);
            int t = llm_runtime_usage_total(rt);

            int64_t elapsed_ms = llm_runtime_usage_elapsed_ms(rt);
            double tps = (elapsed_ms > 0 && c > 0) ? (c * 1000.0 / elapsed_ms) : 0.0;
            char pi[16], co[16], ca[16], to[16];
            printf("\n\033[90min: %s  out: %s",
                   fmt_tokens(p, pi, sizeof(pi)),
                   fmt_tokens(c, co, sizeof(co)));
            if (h > 0) {
                printf("  cached: %s",
                       fmt_tokens(h, ca, sizeof(ca)));
            }
            if (t > 0) {
                printf("  total: %s",
                       fmt_tokens(t, to, sizeof(to)));
            }
            if (tps > 0) {
                printf("  \033[90mtps: %.1f\033[0m", tps);
            }
            /* Context window usage */
            if (ctx && ctx->models) {
                const char *model_id = llm_runtime_get_model(rt);
                const model_entry_t *entry = models_config_find(ctx->models, model_id);
                if (entry && entry->context_window > 0) {
                    int ctxw = entry->context_window;
                    double pct = (ctxw > 0) ? (t * 100.0 / ctxw) : 0;
                    printf("  \033[90mctx: %.1f%%/%s\033[0m",
                           pct, fmt_tokens(ctxw, to, sizeof(to)));
                }
            }
            printf("\033[0m\n");
        }
        printf("\r\033[K");  /* erase any leftover preview */
        if (cb_in_reasoning)  { reasoning_viewport_finish(); cb_in_reasoning = 0; }
        if (cb_in_responding) { printf("\n"); cb_in_responding = 0; }
        break;

    case LLM_RT_EVENT_ERROR:
        if (cb_in_reasoning)  { reasoning_viewport_finish(); }
        cb_in_reasoning  = 0;
        cb_in_responding = 0;
        if (g_content_display_inited)
            md_display_reset(&g_content_display);
        printf("\n\033[1;31mError: %s\033[0m\n", text ? text : "unknown");
        break;

    case LLM_RT_EVENT_USAGE:
        /* Usage stats are displayed in LLM_RT_EVENT_DONE */
        break;
    }
}

/* ============================================================================
 * History Persistence Helper
 * ============================================================================ */
static void save_history_step(cop_context_t *ctx) {
    if (!ctx->db) return;

    const cJSON *history = llm_runtime_get_history(ctx->rt);
    if (!history) return;

    const cJSON *msgs = cJSON_GetObjectItem(history, "messages");
    if (!msgs || cJSON_GetArraySize(msgs) == 0) return;

    if (ctx->session_id < 0) {
        ctx->session_id = history_db_new_session(ctx->db, ctx->cwd);
        if (ctx->session_id < 0) {
            fprintf(stderr, "\n[history_db] failed to create session\n");
            return;
        }
        ctx->saved_count = 0;
    }

    if (history_db_save_step(ctx->db, ctx->session_id, &ctx->saved_count, msgs) != 0) {
        fprintf(stderr, "\n[history_db] failed to save messages\n");
    }
}

/* ============================================================================
 * Command Handlers
 * ============================================================================ */

static void cmd_sessions(cop_context_t *ctx) {
    cJSON *list = history_db_list_sessions(ctx->db, ctx->cwd);
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
        const char *marker = (sid_val == ctx->session_id) ? "\033[32m*\033[0m " : "  ";

        printf("%s%-4d  %-6d  %-19s  %s\n",
               marker, sid_val,
               mc ? mc->valueint : 0,
               cat ? cat->valuestring : "?",
               lum && cJSON_IsString(lum) ? lum->valuestring
                                           : "\033[90m(empty)\033[0m");
    }
    if (n == 0) printf("  \033[90m(no sessions for %s)\033[0m\n", ctx->cwd);
    printf("  \033[90m(* = current)\033[0m\n");
    cJSON_Delete(list);
}

static void cmd_load(cop_context_t *ctx, const char *arg) {
    int64_t sid = strtoll(arg, NULL, 10);
    if (sid <= 0) return;

    cJSON *msgs = history_db_load_session(ctx->db, sid);
    if (!msgs || cJSON_GetArraySize(msgs) == 0) {
        printf("Session %lld not found or empty.\n", (long long)sid);
        cJSON_Delete(msgs);
        return;
    }

    llm_runtime_reset(ctx->rt);
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
            if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
                printf("\033[90m%s\033[0m\n", rc->valuestring);
            }
            if (raw && raw[0]) {
                md_renderer_t *mr = md_renderer_new(md_get_terminal_width());
                sds rendered = md_renderer_feed(mr, raw, (int)strlen(raw));
                printf("%s\033[0m\n", rendered);
                md_renderer_free(mr);
            }
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
            int show = raw ? (int)strlen(raw) : 0;
            if (show > 120) show = 120;
            printf("  \033[90m[%d] %s:\033[0m %.*s%s\n",
                   i + 1, role, show, raw ? raw : "",
                   raw && (int)strlen(raw) > 120 ? "..." : "");
        }
    }
    printf("  \033[90m── end of history ──\033[0m\n\n");

    for (int i = 0; i < n; i++) {
        llm_runtime_add_message(ctx->rt, cJSON_GetArrayItem(msgs, i));
    }
    ctx->session_id = sid;
    ctx->saved_count = history_db_get_saved_count(ctx->db, sid);
    if (ctx->saved_count < 0) ctx->saved_count = 0;
    printf("Loaded session %lld with %d messages.\n", (long long)sid, n);
    cJSON_Delete(msgs);
}

/*
 * /resume — load the most recent session for the current working directory.
 */
static void cmd_resume(cop_context_t *ctx) {
    cJSON *list = history_db_list_sessions(ctx->db, ctx->cwd);
    if (!list || cJSON_GetArraySize(list) == 0) {
        cJSON_Delete(list);
        printf("No sessions found for %s.\n", ctx->cwd);
        return;
    }

    /* Most recent session is first in the list */
    cJSON *first = cJSON_GetArrayItem(list, 0);
    cJSON *id_item = cJSON_GetObjectItem(first, "id");
    if (!id_item) {
        cJSON_Delete(list);
        printf("Error: session list entry missing 'id'.\n");
        return;
    }

    int64_t sid = (int64_t)id_item->valuedouble;
    cJSON_Delete(list);

    char sid_str[32];
    snprintf(sid_str, sizeof(sid_str), "%lld", (long long)sid);
    cmd_load(ctx, sid_str);
}

static void cmd_delete(cop_context_t *ctx, const char *arg) {
    if (!arg || !arg[0]) {
        printf("Usage: /delete <id> | /delete all\n");
        return;
    }
    if (strcmp(arg, "all") == 0) {
        int n = history_db_delete_sessions_by_cwd(ctx->db, ctx->cwd);
        if (n >= 0) {
            ctx->session_id = -1;
            ctx->saved_count = 0;
            llm_runtime_reset(ctx->rt);
            printf("Deleted %d session(s) in %s\n", n, ctx->cwd);
        } else {
            printf("Failed to delete sessions.\n");
        }
        return;
    }

    int64_t did = strtoll(arg, NULL, 10);
    if (did <= 0) return;

    if (history_db_delete_session(ctx->db, did) == 0) {
        printf("Deleted session %lld.\n", (long long)did);
        if (did == ctx->session_id) {
            ctx->session_id = -1;
            ctx->saved_count = 0;
            llm_runtime_reset(ctx->rt);
            printf("(current session was deleted, starting fresh)\n");
        }
    } else {
        printf("Failed to delete session %lld.\n", (long long)did);
    }
}

static void cmd_export(cop_context_t *ctx) {
    const cJSON *history = llm_runtime_get_history(ctx->rt);
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

static void cmd_model(cop_context_t *ctx) {
    if (!ctx->models) {
        printf("No models loaded (check ~/.cop/models.json).\n");
        return;
    }
    printf("\n");
    const char *current = llm_runtime_get_model(ctx->rt);
    const char *last_prov = NULL;
    for (int i = 0; ctx->models[i]; i++) {
        model_entry_t *m = ctx->models[i];
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

static void cmd_set_model(cop_context_t *ctx, const char *model_id) {
    const model_entry_t *entry = models_config_find(ctx->models, model_id);
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
    llm_runtime_set_model(ctx->rt, entry->model_id, entry->api_key, endpoint);
    printf("Switched to \033[1;33m%s\033[0m (\033[36m%s\033[0m)\n",
           entry->model_id, entry->provider);
}

/* ============================================================================
 * REPL
 * ============================================================================ */

coroutine void cop_ui_repl(cop_context_t *ctx) {
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
            break;
        }
        if (strcmp(line, "/sessions") == 0) {
            cmd_sessions(ctx);
            free(line); continue;
        }
        if (strncmp(line, "/load ", 6) == 0) {
            cmd_load(ctx, line + 6);
            free(line); continue;
        }
        if (strncmp(line, "/delete ", 8) == 0) {
            cmd_delete(ctx, line + 8);
            free(line); continue;
        }
        if (strcmp(line, "/model") == 0) {
            cmd_model(ctx);
            free(line); continue;
        }
        if (strcmp(line, "/export") == 0) {
            cmd_export(ctx);
            free(line); continue;
        }
        if (strcmp(line, "/resume") == 0) {
            cmd_resume(ctx);
            free(line); continue;
        }
        if (strncmp(line, "/set_model ", 11) == 0) {
            cmd_set_model(ctx, line + 11);
            free(line); continue;
        }

        /* ── Send message ── */
        fflush(stdout);
        int ret = llm_runtime_send(ctx->rt, line, on_runtime_event, ctx);
        if (llm_runtime_is_cancelled(ctx->rt)) {
            printf("\n[Cancelled]\n");
        }
        if (ret != 0) {
            const char *err = llm_runtime_get_error(ctx->rt);
            printf("\n\033[31mError: %s\033[0m\n", err ? err : "send failed");
        }
        save_history_step(ctx);

        free(line);
        msleep(now() + 100);
    }

    printf("\nGoodbye!\n");
    _exit(0);
}

/* ============================================================================
 * UI Init
 * ============================================================================ */

void cop_ui_init(cop_context_t *ctx) {
    (void)ctx;
    ic_enable_multiline(true);

    /* Expand ~ for isocline history path */
    char hist_path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(hist_path, sizeof(hist_path), "%s/.cop/.history", home);
    ic_set_history(hist_path, 100);

    ic_set_default_completer(completer_wrapper, NULL);
}

void cop_ui_banner(cop_context_t *ctx, const char *model,
                    const char *endpoint, const char *log_file,
                    const char *cwd) {
    printf("Model:    %s\n", model);
    printf("Endpoint: %s\n", endpoint);
    printf("Log:      %s\n", log_file);
    printf("CWD:      %s\n", cwd);
    if (ctx->rt && llm_runtime_is_yolo(ctx->rt))
        printf("Mode:     YOLO — auto-approving all tool calls\n");
    printf("Input:    Enter=newline, Alt+Enter/Ctrl+T=submit\n");
    printf("Commands: /model, /set_model <id>, /sessions, /load, /delete, /export, /resume\n");
}

void cop_ui_sigint(int sig, siginfo_t *info, void *uap) {
    (void)sig; (void)info; (void)uap;
    /*
     * This function runs in signal context.  ONLY async-signal-safe
     * operations are permitted (write, kill, volatile read/write).
     *
     * First Ctrl+C  → cancel the current turn & kill shell child
     * Second Ctrl+C → request clean exit from the main loop
     */
    if (g_cop_ctx && g_cop_ctx->rt && !llm_runtime_is_cancelled(g_cop_ctx->rt)) {
        /* Signal-safe: mark runtime cancelled (volatile int only, no IO/locks) */
        llm_runtime_mark_cancelled(g_cop_ctx->rt);
        /* Kill running shell child process immediately */
        if (g_cop_ctx->running_child_pid > 0) {
            kill(g_cop_ctx->running_child_pid, SIGKILL);
        }
        (void)write(STDOUT_FILENO, "\n[Cancelling...]\n", 17);
    } else if (g_cop_ctx && !g_cop_ctx->want_exit) {
        (void)write(STDOUT_FILENO, "\n[Exiting...]\n", 14);
        g_cop_ctx->want_exit = 1;
    }
}
