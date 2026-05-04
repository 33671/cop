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
#include "libmill/libmill.h"
#include "isocline/include/isocline.h"

/* ============================================================================
 * Global State
 * ============================================================================ */
static llm_runtime_t *g_rt = NULL;  /* set in main() for signal handler access */

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

/* ============================================================================
 * Example Tool: sleep
 * ============================================================================ */
static cJSON *tool_sleep(llm_runtime_t *rt, const cJSON *args) {
    double secs = 1.0;
    cJSON *s = cJSON_GetObjectItem(args, "secs");
    if (s && cJSON_IsNumber(s)) secs = s->valuedouble;

    printf("\n  [tool] sleeping for %.1f seconds...\n", secs);

    /* Sleep in small increments so we can detect cancellation */
    const double step = 0.1;
    double slept = 0;
    while (slept < secs) {
        if (llm_runtime_is_cancelled(rt)) {
            printf("  [tool] cancelled!\n");
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type", "text");
            cJSON_AddStringToObject(result, "text", "cancelled");
            return result;
        }
        usleep((useconds_t)(step * 1000000));
        slept += step;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "done, slept for %.1f seconds", secs);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "text");
    cJSON_AddStringToObject(result, "text", buf);
    return result;
}

/* ============================================================================
 * Example Tool: shell (async CLI runner via llm_runtime_popen)
 * ============================================================================ */
static cJSON *tool_shell(llm_runtime_t *rt, const cJSON *args) {
    cJSON *cmd_json = cJSON_GetObjectItem(args, "cmd");
    const char *cmd = (cmd_json && cJSON_IsString(cmd_json))
                      ? cmd_json->valuestring : NULL;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "text");

    if (!cmd || !*cmd) {
        cJSON_AddStringToObject(result, "text",
            "error: missing 'cmd' argument");
        return result;
    }

    printf("\n  [tool] running: %s\n", cmd);

    char *output = NULL;
    int exit_code = 0;

    /* llm_runtime_popen is coroutine-friendly: uses mfork + fdwait.
     * It does NOT block other coroutines, and is automatically killed
     * if the runtime is cancelled. */
    int ret = llm_runtime_popen(rt, cmd, now() + 30000, &output, &exit_code);

    /* Truncate very long output (both success and failure paths) */
    if (output && *output) {
        size_t len = strlen(output);
        if (len > 4000) {
            output[4000] = '\0';
            strcat(output, "... [truncated]");
        }
    }

    /* Build text field: "output:<output>\nexit_code:<code>" */
    size_t text_len = 128 + (output ? strlen(output) : 0);
    char *text_buf = malloc(text_len);
    if (ret != 0) {
        const char *reason = llm_runtime_is_cancelled(rt)
                             ? "cancelled" : "timed out";
        snprintf(text_buf, text_len,
                 "output:%s\nexit_code:%d\n[WARNING: command %s — partial output above]",
                 output ? output : "(no output)", exit_code, reason);
    } else {
        snprintf(text_buf, text_len, "output:%s\nexit_code:%d",
                 output ? output : "(no output)", exit_code);
    }
    cJSON_AddStringToObject(result, "text", text_buf);
    free(text_buf);
    free(output);
    return result;
}

/* ============================================================================
 * Streaming Callback
 * ============================================================================ */
static void on_runtime_event(llm_runtime_t *rt,
                              llm_runtime_event_t event,
                              const char *text,
                              const cJSON *data,
                              void *user_data) {
    (void)rt;
    (void)user_data;

    switch (event) {
    case LLM_RT_EVENT_REASONING:
        /* Dim for reasoning */
        printf("\033[2;3m%s\033[0m", text);
        fflush(stdout);
        break;

    case LLM_RT_EVENT_CONTENT:
        printf("\033[1;34m%s\033[0m", text);
        fflush(stdout);
        break;

    case LLM_RT_EVENT_STATUS_CHANGE:
        printf("\n\033[90m[%s]\033[0m\n", text ? text : "?");
        break;

    case LLM_RT_EVENT_TOOL_CALLS:
        printf("\n\033[33m[Tool calls detected:");
        if (data && cJSON_IsArray(data)) {
            int n = cJSON_GetArraySize(data);
            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(data, i);
                cJSON *func = cJSON_GetObjectItem(tc, "function");
                cJSON *name = func ? cJSON_GetObjectItem(func, "name") : NULL;
                const char *n_str = (name && cJSON_IsString(name))
                                    ? name->valuestring : "?";
                printf(" %s", n_str);
            }
        }
        printf("]\033[0m\n");
        break;

    case LLM_RT_EVENT_TOOL_RESULT:
        printf("\033[32m  [tool result: %s]\033[0m\n", text ? text : "?");
        break;

    case LLM_RT_EVENT_USAGE:
        if (data) {
            cJSON *p = cJSON_GetObjectItem(data, "prompt_tokens");
            cJSON *c = cJSON_GetObjectItem(data, "completion_tokens");
            cJSON *h = cJSON_GetObjectItem(data, "cached_tokens");
            cJSON *t = cJSON_GetObjectItem(data, "total_tokens");
            printf("\n\033[90m[Usage: prompt=%d, completion=%d, cached=%d, total=%d]\033[0m\n",
                   p && cJSON_IsNumber(p) ? p->valueint : 0,
                   c && cJSON_IsNumber(c) ? c->valueint : 0,
                   h && cJSON_IsNumber(h) ? h->valueint : 0,
                   t && cJSON_IsNumber(t) ? t->valueint : 0);
        }
        break;

    case LLM_RT_EVENT_DONE:
        printf("\n\033[90m[Done]\033[0m\n");
        break;

    case LLM_RT_EVENT_ERROR:
        printf("\n\033[31m[Error: %s]\033[0m\n", text ? text : "unknown");
        break;
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
        if (strcmp(line, "reset") == 0) {
            llm_runtime_reset(rt);
            printf("Conversation reset.\n");
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
            } else {
                const char *err = llm_runtime_get_error(rt);
                printf("\n\033[31mError: %s\033[0m\n", err ? err : "send failed");
            }
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
    /* Load .env */
    load_env_file(".env");
    load_env_file("../.env");

    signal(SIGINT, sigint_handler);

    /* Config */
    const char *model       = "kimi-k2.5";
    const char *api_endpoint = "https://api.moonshot.cn/v1/chat/completions";
    const char *log_file    = "llm_runtime.log";

    /* API key */
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key) api_key = getenv("MOONSHOT_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Error: set OPENAI_API_KEY or MOONSHOT_API_KEY\n");
        return 1;
    }

    /* Read base URL and model from env */
    const char *api_url = getenv("OPENAI_BASE_URL");
    if (api_url) {
        static char endpoint_buf[512];
        snprintf(endpoint_buf, sizeof(endpoint_buf),
                 "%s/chat/completions", api_url);
        api_endpoint = endpoint_buf;
    }
    const char *env_model = getenv("MODEL_NAME");
    if (env_model) model = env_model;

    /* Parse CLI args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc)
            api_endpoint = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_file = argv[++i];
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Configure isocline */
    ic_enable_multiline(true);         /* Enter=submit, Shift+Enter=newline */
    ic_set_history(".history", 100);   /* persistent history */

    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║   LLM Runtime Demo (isocline)                         ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║ Model:    %-46s ║\n", model);
    printf("║ Endpoint: %-44s ║\n", api_endpoint);
    printf("║ Log:      %-44s ║\n", log_file);
    printf("║ Input:    Enter=submit, Shift+Enter/Ctrl+J=newline    ║\n");
    printf("║ Commands: quit, reset                                 ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n\n");

    /* Create runtime */
    llm_runtime_t *rt = llm_runtime_new(api_key, model, api_endpoint, log_file);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        curl_global_cleanup();
        return 1;
    }

    /* Register tools and schemas */
    llm_runtime_register_tool(rt, "sleep", tool_sleep);
    llm_runtime_register_tool(rt, "shell", tool_shell);

    /* sleep tool schema */
    cJSON *sleep_schema = cJSON_CreateObject();
    cJSON_AddStringToObject(sleep_schema, "type", "function");
    cJSON *func = cJSON_AddObjectToObject(sleep_schema, "function");
    cJSON_AddStringToObject(func, "name", "sleep");
    cJSON_AddStringToObject(func, "description", "Sleep for a given number of seconds");
    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *secs = cJSON_AddObjectToObject(props, "secs");
    cJSON_AddStringToObject(secs, "type", "number");
    cJSON_AddStringToObject(secs, "description", "Number of seconds to sleep");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("secs"));

    /* shell tool schema */
    cJSON *shell_schema = cJSON_CreateObject();
    cJSON_AddStringToObject(shell_schema, "type", "function");
    cJSON *sh_func = cJSON_AddObjectToObject(shell_schema, "function");
    cJSON_AddStringToObject(sh_func, "name", "shell");
    cJSON_AddStringToObject(sh_func, "description",
        "Run a shell command and return its output (stdout+stderr merged). "
        "Useful for: ls, cat, grep, find, wc, date, curl, git status, etc. "
        "Avoid commands that run forever or require interactive input.");
    cJSON *sh_params = cJSON_AddObjectToObject(sh_func, "parameters");
    cJSON_AddStringToObject(sh_params, "type", "object");
    cJSON *sh_props = cJSON_AddObjectToObject(sh_params, "properties");
    cJSON *sh_cmd = cJSON_AddObjectToObject(sh_props, "cmd");
    cJSON_AddStringToObject(sh_cmd, "type", "string");
    cJSON_AddStringToObject(sh_cmd, "description", "The shell command to execute");
    cJSON *sh_required = cJSON_AddArrayToObject(sh_params, "required");
    cJSON_AddItemToArray(sh_required, cJSON_CreateString("cmd"));

    cJSON *schemas = cJSON_CreateArray();
    cJSON_AddItemToArray(schemas, sleep_schema);
    cJSON_AddItemToArray(schemas, shell_schema);
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
