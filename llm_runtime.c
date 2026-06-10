/*
 * llm_runtime.c
 *
 * High-level LLM runtime implementation.
 * Lifecycle, configuration, tool registration, and async subprocess.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include "sds/sds.h"
#include "llm_runtime.h"
#include "llm_runtime_internal.h"
#include "agent_loop.h"
#include "cjson_arena.h"
#include "utils.h"
#include "debug.h"

/* ============================================================================
 * Lifecycle
 * ============================================================================ */
llm_runtime_t *llm_runtime_new(const char *api_key, const char *model,
                                const char *api_endpoint, const char *log_file) {
    if (!api_key || !model) return NULL;

    llm_runtime_t *rt = calloc(1, sizeof(llm_runtime_t));
    if (!rt) return NULL;

    rt->api_key        = sdsnew(&rt->arena, api_key);
    rt->model          = sdsnew(&rt->arena, model);
    rt->api_endpoint   = sdsnew(&rt->arena, api_endpoint ? api_endpoint
                             : "https://api.moonshot.cn/v1/chat/completions");
    rt->log_file       = log_file ? sdsnew(&rt->arena, log_file) : NULL;
    rt->running        = 1;

    /* Create stream client */
    rt->client = stream_client_new(api_key, model, api_endpoint, log_file);
    if (!rt->client) {
        set_error(rt, "stream_client_new failed");
        llm_runtime_free(rt);
        return NULL;
    }

    /* Create message parser */
    rt->parser = llm_parser_create();
    if (!rt->parser) {
        set_error(rt, "llm_parser_create failed");
        llm_runtime_free(rt);
        return NULL;
    }

    return rt;
}

void llm_runtime_free(llm_runtime_t *rt) {
    if (!rt) return;

    /* Cancel any active request */
    if (rt->client) stream_client_free(rt->client);
    if (rt->parser) llm_parser_destroy(rt->parser);

    arena_free(&rt->arena);
    free(rt);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */
void llm_runtime_set_system_message(llm_runtime_t *rt, const char *msg) {
    if (!rt || !msg) return;
    stream_client_set_system_message(rt->client, msg);
}

void llm_runtime_set_temperature(llm_runtime_t *rt, double temp) {
    if (!rt) return;
    stream_client_set_temperature(rt->client, temp);
}

void llm_runtime_set_max_tokens(llm_runtime_t *rt, int max_tokens) {
    if (!rt) return;
    stream_client_set_max_tokens(rt->client, max_tokens);
}

int llm_runtime_set_model(llm_runtime_t *rt, const char *model,
                            const char *api_key, const char *api_endpoint) {
    if (!rt || !model) return -1;
    stream_client_set_model(rt->client, model);
    stream_client_set_api(rt->client, api_key, api_endpoint);
    rt->model = sdsnew(&rt->arena, model);
    if (api_key)     { rt->api_key      = sdsnew(&rt->arena, api_key); }
    if (api_endpoint) { rt->api_endpoint = sdsnew(&rt->arena, api_endpoint); }
    return 0;
}

const char *llm_runtime_get_model(llm_runtime_t *rt) {
    return rt ? rt->model : NULL;
}

/* ============================================================================
 * Tool Registration
 * ============================================================================ */
int llm_runtime_register_tool(llm_runtime_t *rt, const char *name,
                               llm_tool_fn_t fn) {
    if (!rt || !name || !fn) return -1;
    if (rt->tool_count >= LLM_RUNTIME_MAX_TOOLS) return -1;

    /* Check for duplicates */
    for (int i = 0; i < rt->tool_count; i++) {
        if (strcmp(rt->tools[i].name, name) == 0) return -1;
    }

    strncpy(rt->tools[rt->tool_count].name, name,
            sizeof(rt->tools[rt->tool_count].name) - 1);
    rt->tools[rt->tool_count].name[sizeof(rt->tools[rt->tool_count].name) - 1] = '\0';
    rt->tools[rt->tool_count].fn = fn;
    rt->tool_count++;
    return 0;
}

void llm_runtime_set_tool_schema(llm_runtime_t *rt, const cJSON *schemas) {
    if (!rt) return;
    stream_client_set_tool_schemas(rt->client, schemas);
}

/* ============================================================================
 * Conversation
 * ============================================================================ */
int llm_runtime_add_user_message(llm_runtime_t *rt, const char *text) {
    if (!rt || !text) return -1;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", text);

    LlmParserStatus ret = llm_parser_add_message(rt->parser, msg);
    cJSON_Delete(msg);

    return (ret < 0) ? -1 : 0;
}

int llm_runtime_add_message(llm_runtime_t *rt, const cJSON *msg) {
    if (!rt || !msg) return -1;

    LlmParserStatus ret = llm_parser_add_message(rt->parser, msg);
    return (ret < 0) ? -1 : 0;
}

/* ============================================================================
 * Streaming Send (thin wrapper → agent_loop.c)
 * ============================================================================ */
coroutine int llm_runtime_send(llm_runtime_t *rt,
                                const char *user_text,
                                llm_runtime_callback_t on_chunk,
                                void *user_data) {
    return agent_loop_run(rt, user_text, on_chunk, user_data);
}

/* ============================================================================
 * Utilities
 * ============================================================================ */
const cJSON *llm_runtime_get_history(const llm_runtime_t *rt) {
    if (!rt) return NULL;
    return llm_parser_get_history(rt->parser);
}

void llm_runtime_reset(llm_runtime_t *rt) {
    if (!rt) return;
    llm_parser_reset(rt->parser);
    rt->has_error = 0;
    rt->error_msg[0] = '\0';
}

void llm_runtime_cancel(llm_runtime_t *rt) {
    if (!rt) return;
    rt->running = 0;
    stream_client_cancel(rt->client);
}

void llm_runtime_set_child_pid_ptr(llm_runtime_t *rt, volatile pid_t *pid_ptr) {
    if (!rt) return;
    rt->child_pid_ptr = pid_ptr;
}

void llm_runtime_mark_cancelled(llm_runtime_t *rt) {
    if (!rt) return;
    rt->running = 0;
}

const char *llm_runtime_get_error(const llm_runtime_t *rt) {
    if (!rt || !rt->has_error) return NULL;
    return rt->error_msg;
}

const char *llm_runtime_get_state_string(const llm_runtime_t *rt) {
    if (!rt || !rt->client) return "null";
    return stream_client_get_state_string(rt->client);
}

int llm_runtime_is_cancelled(const llm_runtime_t *rt) {
    return rt ? !rt->running : 1;
}

void llm_runtime_set_yolo(llm_runtime_t *rt, int yolo) {
    if (rt) rt->yolo = yolo;
}

int llm_runtime_is_yolo(const llm_runtime_t *rt) {
    return rt ? rt->yolo : 0;
}

/* ============================================================================
 * Usage Statistics Accessors
 * ============================================================================ */
int  llm_runtime_usage_seen(const llm_runtime_t *rt)        { return rt ? rt->usage_seen : 0; }
int  llm_runtime_usage_prompt(const llm_runtime_t *rt)       { return rt ? rt->usage_prompt : 0; }
int  llm_runtime_usage_completion(const llm_runtime_t *rt)   { return rt ? rt->usage_completion : 0; }
int  llm_runtime_usage_total(const llm_runtime_t *rt)        { return rt ? rt->usage_total : 0; }
int  llm_runtime_usage_cached(const llm_runtime_t *rt)       { return rt ? rt->usage_cached : -1; }
int64_t llm_runtime_usage_elapsed_ms(const llm_runtime_t *rt)  { return rt ? rt->usage_elapsed_ms : 0; }

/* ============================================================================
 * Async Subprocess (coroutine-friendly popen)
 * ============================================================================ */

coroutine int llm_runtime_popen(llm_runtime_t *rt,
                                 const char *cmd,
                                 int64_t deadline,
                                 char **output,
                                 int *exit_code)
{
    if (!rt || !cmd || !output || !exit_code) return -1;

    *output = NULL;
    *exit_code = -1;

    /* Create pipe for capturing merged stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = mfork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* ================================================================
         * CHILD: redirect stdout+stderr → pipe, exec /bin/sh -c <cmd>
         * ================================================================ */
        setsid();                                /* new session: detach from controlling tty,
                                                   also isolates process group */
        /* Redirect stdin from /dev/null to prevent terminal programs
         * (vim, less, etc.) from writing escape sequences to the tty. */
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }
        close(pipefd[0]);                        /* close read end */
        dup2(pipefd[1], STDOUT_FILENO);        /* stdout → pipe write */
        dup2(pipefd[1], STDERR_FILENO);        /* stderr → pipe write */
        close(pipefd[1]);                      /* close original write fd */

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);

        /* If exec fails */
        _exit(127);
    }

    /* ================================================================
     * PARENT: read from pipe via fdwait(), check cancellation, accumulate
     * ================================================================ */
    close(pipefd[1]);  /* close write end */

    /* Make read end non-blocking for fdwait */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    if (rt->child_pid_ptr) *rt->child_pid_ptr = pid;  /* expose for signal handler */

    size_t   out_cap  = 4096;
    size_t   out_len  = 0;
    char    *out_buf  = malloc(out_cap);
    int      finished = 0;

    if (!out_buf) goto cleanup_error;
    out_buf[0] = '\0';

    /*
     * Hard safety limit: maximum iterations ≈ (deadline_ms / 100) + 100.
     * Prevents infinite looping if now() or fdwait misbehave.
     */
    int64_t max_iter = (deadline >= 0) ? ((deadline - now()) / 100 + 100) : 600;
    if (max_iter < 100) max_iter = 100;
    int64_t iter = 0;

    while (!finished && !llm_runtime_is_cancelled(rt)) {
        /* Check deadline (both time-based and iteration-based) */
        if (deadline >= 0 && now() >= deadline) {
            break;
        }
        if (++iter > max_iter) {
            break;
        }

        int64_t wait_deadline = now() + 100;  /* 100ms poll interval */
        if (deadline >= 0 && wait_deadline > deadline) {
            wait_deadline = deadline;
        }

        int ev = fdwait(pipefd[0], FDW_IN, wait_deadline);

        /*
         * Check deadline again after fdwait returns, in case fdwait
         * returned early (spurious wakeup / zero timeout).
         */         
        if (deadline >= 0 && now() >= deadline) {
            break;
        }
        /* Drain data from pipe (FDW_ERR may accompany FDW_IN on EOF) */
        if (ev & FDW_IN) {
            ssize_t r = pipe_drain(pipefd[0], &out_buf, &out_len, &out_cap);
            if (r == 0) { finished = 1; }
            else if (r == -2) goto cleanup_error;
            /* r>0 (data) or r==-1 (EAGAIN): continue */
        }

        /* FDW_ERR without FDW_IN: child may have exited and closed the pipe.
         * Try one more read — read() returns 0 for EOF. */
        if ((ev & FDW_ERR) && !(ev & FDW_IN)) {
            ssize_t r = pipe_drain(pipefd[0], &out_buf, &out_len, &out_cap);
            if (r == 0) { finished = 1; }
            else if (r == -2) goto cleanup_error;
            else if (r == -1) break;  /* no data on hangup → stop */
        }
    }

    /* If not finished cleanly, kill the child */
    if (!finished) {
        kill(pid, SIGKILL);
    }

    /* Reap the child */
    int status = 0;
    waitpid(pid, &status, 0);

    /* Clean up pipe and clear child PID */
    fdclean(pipefd[0]);
    close(pipefd[0]);
    if (rt->child_pid_ptr) *rt->child_pid_ptr = -1;

    /* Strip trailing newline if present */
    if (out_len > 0 && out_buf[out_len - 1] == '\n') {
        out_buf[--out_len] = '\0';
    }

    /* Always return accumulated output — even on timeout/cancellation */
    *output    = out_buf;
    *exit_code = finished && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return finished ? 0 : -1;

cleanup_error:
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    fdclean(pipefd[0]);
    close(pipefd[0]);
    free(out_buf);  /* NULL-safe */
    if (rt->child_pid_ptr) *rt->child_pid_ptr = -1;
    *output = NULL;
    *exit_code = -1;
    return -1;
}
