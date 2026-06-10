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
 * Application Context (replaces scattered global variables)
 * ============================================================================ */

typedef struct cop_context {
    llm_runtime_t        *rt;
    history_db_t         *db;
    int64_t               session_id;
    int                   saved_count;
    char                  cwd[4096];
    model_entry_t       **models;
    volatile sig_atomic_t want_exit;
    volatile pid_t        running_child_pid;
} cop_context_t;

/*
 * Single global pointer to the context, used ONLY by the signal handler
 * (cop_ui_sigint).  Set by main() before launching the REPL.
 *
 * Direct access to ctx fields outside of cop_ui_sigint is discouraged —
 * pass cop_context_t * explicitly instead.
 */
extern cop_context_t *g_cop_ctx;

/* ============================================================================
 * UI Lifecycle
 * ============================================================================ */

/* Initialize terminal UI: completion, history, multiline. */
void cop_ui_init(cop_context_t *ctx);

/* Print startup banner. */
void cop_ui_banner(cop_context_t *ctx, const char *model,
                    const char *endpoint, const char *log_file,
                    const char *cwd);

/* The main REPL coroutine. Call with go(cop_ui_repl(&ctx)). */
coroutine void cop_ui_repl(cop_context_t *ctx);

/* SIGINT handler — cancel current turn or exit.
 * Only touches volatile fields of g_cop_ctx and uses async-signal-safe
 * operations (write, kill). */
void cop_ui_sigint(int sig, siginfo_t *info, void *uap);

#ifdef __cplusplus
}
#endif

#endif /* COP_UI_H */
