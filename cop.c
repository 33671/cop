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
#include "tool_functions.h"
#include "history_db.h"
#include "models_config.h"
#include "cop_ui.h"
#include "libmill/libmill.h"

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

    /* Ensure ~/.cop/ exists before loading config */
    {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        char cop_dir[512];
        snprintf(cop_dir, sizeof(cop_dir), "%s/.cop", home);
        mkdir(cop_dir, 0755);
    }

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
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
        snprintf(log_file, sizeof(log_file), "%s/.cop/cop_%s.log", home, ts);
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

    /* Create runtime */
    llm_runtime_t *rt = llm_runtime_new(api_key, model, api_endpoint, log_file);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        curl_global_cleanup();
        return 1;
    }
    if (yolo_flag) llm_runtime_set_yolo(rt, 1);

    /* Set max_tokens from model config (0 = let API use default) */
    if (default_model->max_tokens > 0)
        llm_runtime_set_max_tokens(rt, default_model->max_tokens);

    /* Capture working directory */
    if (!getcwd(app.cwd, sizeof(app.cwd))) app.cwd[0] = '\0';

    /* Open history DB (session created lazily on first message) */
    history_db_open(&app.db);

    /* Register tools */
    llm_runtime_register_tool(rt, "sleep", tool_sleep);
    llm_runtime_register_tool(rt, "shell", tool_shell);
    llm_runtime_register_tool(rt, "read",  tool_read);
    llm_runtime_register_tool(rt, "write", tool_write);
    llm_runtime_register_tool(rt, "edit",  tool_edit);

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
    curl_global_cleanup();
    return 0;
}
