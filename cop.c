/*
 * cop.c
 *
 * cop - async AI coding agent in C.
 * Main entry point: loads config, creates runtime, launches REPL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>

#include "llm_runtime.h"
#include "utils.h"
#include "tool_functions.h"
#include "history_db.h"
#include "models_config.h"
#include "cop_ui.h"
#include "libmill/libmill.h"
#include "sds/sds.h"
#include "cop_lua.h"

/* ============================================================================
 * Global context pointer - for signal handler access only
 * ============================================================================ */
cop_context_t *g_cop_ctx = NULL;

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char *argv[]) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cop_ui_sigint;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Allocate application context on stack */
    cop_context_t app;
    memset(&app, 0, sizeof(app));
    app.session_id = -1;
    g_cop_ctx = &app;

    /* Load model config from ~/.cop/models.json */
    app.models = models_config_load();
    if (!app.models || !app.models[0]) {
        fprintf(stderr, "Error: no models found in ~/.cop/models.json\n"
                "Create it with format:\n"
                "{\"providers\":{\"name\":{\"baseUrl\":\"...\","
                "\"apiKey\":\"...\",\"models\":[{\"id\":\"...\","
                "\"contextWindow\":1000000}]}}}\n");
        return 1;
    }

    /* Default: first model in config */
    const model_entry_t *default_model = app.models[0];
    const char *model   = default_model->model_id;
    const char *api_key = default_model->api_key;
    const char *base_url = default_model->base_url;

    /* Log file: ~/.cop/cop_YYYYMMDD_HHMMSS.log */
    char log_file[512];
    {
        char *cop_dir = expand_tilde("~/.cop");
        if (!cop_dir) cop_dir = strdup("/tmp/.cop");
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
        snprintf(log_file, sizeof(log_file), "%s/cop_%s.log", cop_dir, ts);
        free(cop_dir);
    }

    /* Check saved model preference from ~/.cop/model_set */
    {
        const model_entry_t *saved = models_config_load_saved(app.models);
        if (saved) {
            model    = saved->model_id;
            api_key  = saved->api_key;
            base_url = saved->base_url;
        }
    }

    /* Parse CLI args */
    int yolo_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            const model_entry_t *entry =
                models_config_find(app.models, argv[i + 1]);
            if (entry) {
                model    = entry->model_id;
                api_key  = entry->api_key;
                base_url = entry->base_url;
            } else {
                model = argv[i + 1];  /* unknown, pass through as-is */
            }
            i++;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            snprintf(log_file, sizeof(log_file), "%s", argv[++i]);
        else if (strcmp(argv[i], "--yolo") == 0)
            yolo_flag = 1;
    }

    /* Build API endpoint from chosen base_url */
    char api_endpoint[512];
    snprintf(api_endpoint, sizeof(api_endpoint),
             "%s/chat/completions", base_url);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Init terminal UI */
    cop_ui_init(&app);

    /* Capture working directory */
    if (!getcwd(app.cwd, sizeof(app.cwd))) app.cwd[0] = '\0';

    /* Build system prompt with current working directory appended */
    Arena sys_arena = {0};
    sds system_prompt = sdsempty(&sys_arena);
    system_prompt = sdscat(system_prompt,
        "You are an expert assistant operating inside an agent harness on Linux. You help users by reading files,"
        "executing shell commands, editing code, and writing new files.\n"
        "\nAvailable Tools:\n"
        "- `shell`: Execute any shell command (bash, ls, grep, find, etc.). For long-running commands,\n"
        "prefix with `timeout` to control execution time.\n"
        "- `read`: Read file contents with offset/limit support. Use this to view source code, logs, config\n"
        "files, or any text data.\n"
        "- `write`: Create new files or overwrite existing ones. Parent directories are created\n"
        "automatically. Append mode also supported.\n"
        "- `edit`: Perform precise substring replacements in existing files. Supports single or replace-all\n"
        "modes. Use for targeted changes rather than full rewrites.\n"
        "- `sleep`: Pause execution for a specified number of seconds (useful for timing or waiting).\n"
        "- `lua`: Execute Lua code in the embedded interpreter. Exposes all standard Lua libraries\n"
        "  (math, string, table, io, os, coroutine, utf8, debug, package) plus custom helpers:\n"
        "  cop.pwd(), cop.ls([path]), cop.read_file(path), cop.write_file(path, content),\n"
        "  cop.shell(cmd).\n"
        "  Built-in SQLite (no require needed): cop.sqlite_open([path]) -> db_handle,\n"
        "  cop.sqlite_close(db), cop.sqlite_exec(db, sql) -> {rows_affected,last_insert_id},\n"
        "  cop.sqlite_get(db, sql) -> array of row-tables,\n"
        "  cop.sqlite_get_one(db, sql) -> one row or nil.\n"
        "  Use print() for output; it is captured and returned.\n"
        "  Note: ~/.cop/history.db is a SQLite database containing chat history;\n"
        "  you can query it with cop.sqlite_get() when needed.\n"
        "Guidelines:\n"
        "- Use `shell` for exploration (`ls`, `find`, `grep`, `cat`, etc.) before reading or editing files.\n"
        "- Use `read` to examine file contents instead of `cat` or `sed` in shell.\n"
        "- Use `edit` for targeted changes — keep `old` text as short and unique as possible within the\n"
        "file.\n"
        "- Use `write` for new files or complete rewrites (when `edit` would be impractical).\n"
        "- Be concise in responses and show file paths clearly when working with files.\n"
        "- When you need to check environment state (current directory, file existence, etc.), use `shell`\n"
        "commands first.\n"
        "- Prefer making multiple independent changes in parallel when they don't depend on each other.\n"
        "Response Style:\n"
        "- Be helpful, direct, and actionable.\n"
        "- Explain what you're doing and why, but keep explanations succinct.\n"
        "- When in doubt, show the relevant output or code context.\n"
        "\nCurrent working directory: ");
    system_prompt = sdscat(system_prompt, app.cwd);

    /* Create runtime */
    llm_runtime_t *rt = llm_runtime_new(api_key, model, api_endpoint, log_file,
        system_prompt);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        curl_global_cleanup();
        return 1;
    }
    if (yolo_flag) llm_runtime_set_yolo(rt, 1);

    /* Set max_tokens from model config (0 = let API use default) */
    if (default_model->max_tokens > 0)
        llm_runtime_set_max_tokens(rt, default_model->max_tokens);

    /* Open history DB (session created lazily on first message) */
    history_db_open(&app.db);

    /* Register tools */
    llm_runtime_register_tool(rt, "sleep", tool_sleep);
    llm_runtime_register_tool(rt, "shell", tool_shell);
    llm_runtime_register_tool(rt, "read",  tool_read);
    llm_runtime_register_tool(rt, "write", tool_write);
    llm_runtime_register_tool(rt, "edit",  tool_edit);
    llm_runtime_register_tool(rt, "lua",   tool_lua);

    cJSON *schemas = tool_functions_create_schema();
    llm_runtime_set_tool_schema(rt, schemas);
    cJSON_Delete(schemas);

    /* Wire runtime into context & expose child PID for signal handler */
    app.rt = rt;
    app.running_child_pid = -1;
    llm_runtime_set_child_pid_ptr(rt, &app.running_child_pid);

    cop_ui_banner(&app, model, api_endpoint, log_file, app.cwd);

    /* Launch REPL */
    go(cop_ui_repl(&app));

    /* Sleep until SIGINT/SIGTERM requests exit */
    while (!app.want_exit) {
        msleep(now() + 250);
    }

    /* Clean up */
    history_db_close(app.db);
    llm_runtime_free(rt);
    cop_lua_cleanup();
    curl_global_cleanup();
    return 0;
}
