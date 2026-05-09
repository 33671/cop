/*
 * cop.c
 *
 * cop — async AI coding agent in C.
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
 * Global State (shared with cop_ui.c)
 * ============================================================================ */
llm_runtime_t    *g_rt = NULL;
history_db_t     *g_db = NULL;
int64_t           g_session_id = -1;
int               g_saved_count = 0;
char              g_cwd[4096];
model_entry_t   **g_models = NULL;

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char *argv[]) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cop_ui_sigint;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGINT, &sa, NULL);

    /* Load model config from ~/.cop/models.json */
    g_models = models_config_load();
    if (!g_models || !g_models[0]) {
        fprintf(stderr, "Error: no models found in ~/.cop/models.json\n"
                "Create it with format:\n"
                "{\"providers\":{\"name\":{\"baseUrl\":\"...\","
                "\"apiKey\":\"...\",\"models\":[{\"id\":\"...\"}]}}}\n");
        return 1;
    }

    /* Default: first model in config */
    const char *model   = g_models[0]->model_id;
    const char *api_key = g_models[0]->api_key;
    char api_endpoint[512];
    snprintf(api_endpoint, sizeof(api_endpoint),
             "%s/chat/completions", g_models[0]->base_url);

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

    /* Parse CLI args */
    int yolo_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            snprintf(log_file, sizeof(log_file), "%s", argv[++i]);
        else if (strcmp(argv[i], "--yolo") == 0)
            yolo_flag = 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Init terminal UI */
    cop_ui_init();

    /* Ensure ~/.cop/ exists */
    {
        const char *home = getenv("HOME");
        char cop_dir[512];
        snprintf(cop_dir, sizeof(cop_dir), "%s/.cop", home ? home : "/tmp");
        mkdir(cop_dir, 0755);
    }

    /* Create runtime */
    llm_runtime_t *rt = llm_runtime_new(api_key, model, api_endpoint, log_file);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        curl_global_cleanup();
        return 1;
    }
    if (yolo_flag) llm_runtime_set_yolo(rt, 1);

    /* Set max_tokens from model config (0 = let API use default) */
    if (g_models[0]->max_tokens > 0)
        llm_runtime_set_max_tokens(rt, g_models[0]->max_tokens);

    /* Capture working directory */
    if (!getcwd(g_cwd, sizeof(g_cwd))) g_cwd[0] = '\0';

    /* Open history DB (session created lazily on first message) */
    history_db_open(&g_db);

    /* Register tools */
    llm_runtime_register_tool(rt, "sleep", tool_sleep);
    llm_runtime_register_tool(rt, "shell", tool_shell);
    llm_runtime_register_tool(rt, "read",  tool_read);
    llm_runtime_register_tool(rt, "write", tool_write);
    llm_runtime_register_tool(rt, "edit",  tool_edit);

    cJSON *schemas = tool_functions_create_schema();
    llm_runtime_set_tool_schema(rt, schemas);
    cJSON_Delete(schemas);

    /* Set global for SIGINT */
    g_rt = rt;

    cop_ui_banner(model, api_endpoint, log_file, g_cwd);

    /* Launch REPL */
    go(cop_ui_repl(rt));

    while (1) msleep(now() + 100);

    /* Not reached */
    llm_runtime_free(rt);
    curl_global_cleanup();
    return 0;
}
