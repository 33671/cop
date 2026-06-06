/*
 * cop_ui.h
 *
 * Terminal UI layer for cop: REPL, completion, streaming display.
 */

#ifndef COP_UI_H
#define COP_UI_H

#include <signal.h>
#include "llm_runtime.h"
#include "history_db.h"
#include "models_config.h"
#include "libmill/libmill.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Globals (owned by cop.c, accessible from cop_ui.c)
 * ============================================================================ */
extern llm_runtime_t        *g_rt;
extern history_db_t         *g_db;
extern int64_t               g_session_id;
extern int                   g_saved_count;
extern char                  g_cwd[4096];
extern model_entry_t       **g_models;
extern volatile sig_atomic_t g_want_exit;
extern volatile pid_t        g_running_child_pid;

/* ============================================================================
 * UI Lifecycle
 * ============================================================================ */

/* Initialize terminal UI: completion, history, multiline. */
void cop_ui_init(void);

/* Print startup banner. */
void cop_ui_banner(const char *model, const char *endpoint,
                    const char *log_file, const char *cwd);

/* The main REPL coroutine. Call with go(cop_ui_repl(rt)). */
coroutine void cop_ui_repl(llm_runtime_t *rt);

/* SIGINT handler — cancel current turn or exit. */
void cop_ui_sigint(int sig, siginfo_t *info, void *uap);

#ifdef __cplusplus
}
#endif

#endif /* COP_UI_H */
