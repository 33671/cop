/*
 * tool_functions.c
 *
 * Example tool function implementations and schema helpers for the LLM runtime.
 *
 * These demonstrate:
 *   - Cancellation-aware tool execution (sleep, shell)
 *   - Async subprocess execution via llm_runtime_popen() (shell)
 *   - File read/write/edit operations with user approval (read, write, edit)
 *   - Proper cJSON result format for tool messages
 *   - Tool schema construction helpers
 */

#include "tool_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include "isocline/include/isocline.h"

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Maximum file size we'll read (100 KB) */
#define TOOL_READ_MAX_BYTES (100 * 1024)

/* Prompt user for y/N approval. Returns 1 if approved, 0 otherwise. */
static int ask_approval(const char *prompt) {
    char *reply = ic_readline(prompt);
    if (!reply) return 0;

    size_t rlen = strlen(reply);
    while (rlen > 0 && (reply[rlen-1] == '\n' || reply[rlen-1] == '\r'))
        reply[--rlen] = '\0';

    int approved = (rlen > 0 && (reply[0] == 'y' || reply[0] == 'Y'));
    free(reply);
    return approved;
}

/* Create parent directories for a given path (in-place modification of copy ok) */
static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return 0;
}

/* ============================================================================
 * Sleep Tool
 * ============================================================================ */

cJSON *tool_sleep(llm_runtime_t *rt, const cJSON *args) {
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
 * Shell Tool (async CLI runner via llm_runtime_popen)
 * ============================================================================ */

cJSON *tool_shell(llm_runtime_t *rt, const cJSON *args) {
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

    /* Ask for user approval before executing */
    printf("\n  [tool] proposed command: %s\n", cmd);
    if (!ask_approval("[yellow][b]Run this command? y/N[/] ")) {
        cJSON_AddStringToObject(result, "text",
            "user denied shell execution");
        return result;
    }

    printf("  [tool] running: %s\n", cmd);

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

    /* Build text field */
    size_t text_len = 128 + (output ? strlen(output) : 0);
    char *text_buf = malloc(text_len);
    if (ret != 0) {
        const char *reason = llm_runtime_is_cancelled(rt)
                             ? "cancelled" : "timed out";
        snprintf(text_buf, text_len,
                 "Output:\n%s\nExit_code:%d\n[WARNING: command %s partial output above]",
                 output ? output : "(no output)", exit_code, reason);
    } else {
        snprintf(text_buf, text_len, "Output:\n%s\nExit_code:%d",
                 output ? output : "(no output)", exit_code);
    }
    cJSON_AddStringToObject(result, "text", text_buf);
    free(text_buf);
    free(output);
    return result;
}

/* ============================================================================
 * Read Tool
 * ============================================================================ */

cJSON *tool_read(llm_runtime_t *rt, const cJSON *args) {
    (void)rt;

    cJSON *p = cJSON_GetObjectItem(args, "path");
    const char *path = (p && cJSON_IsString(p)) ? p->valuestring : NULL;

    /* Parse offset (1-indexed, default 1) */
    cJSON *off = cJSON_GetObjectItem(args, "offset");
    int offset = 1;
    if (off && cJSON_IsNumber(off)) {
        int v = off->valueint;
        if (v < 1) v = 1;
        offset = v;
    }

    /* Parse limit (default 1000, max 1000) */
    cJSON *lim = cJSON_GetObjectItem(args, "limit");
    int limit = 1000;
    if (lim && cJSON_IsNumber(lim)) {
        int v = lim->valueint;
        if (v < 1) v = 1;
        if (v > 1000) v = 1000;
        limit = v;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "text");

    if (!path || !*path) {
        cJSON_AddStringToObject(result, "text",
            "error: missing 'path' argument");
        return result;
    }

    printf("\n  [tool] reading: %s (offset=%d, limit=%d)\n", path, offset, limit);

    FILE *f = fopen(path, "rb");
    if (!f) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf),
                 "error: cannot open '%s': %s", path, strerror(errno));
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        cJSON_AddStringToObject(result, "text",
            "error: cannot determine file size");
        return result;
    }

    /* Read up to TOOL_READ_MAX_BYTES */
    size_t read_size = (file_size < TOOL_READ_MAX_BYTES)
                       ? (size_t)file_size
                       : (size_t)TOOL_READ_MAX_BYTES;

    char *buf = malloc(read_size + 1);
    if (!buf) {
        fclose(f);
        cJSON_AddStringToObject(result, "text",
            "error: memory allocation failed");
        return result;
    }

    size_t nread = fread(buf, 1, read_size, f);
    fclose(f);
    buf[nread] = '\0';

    /* Walk through lines, collecting from offset for limit lines */
    int line_count = 0;
    size_t collected = 0;
    size_t cap = nread;                        /* max output bytes */
    char *out = malloc(cap + 256);             /* extra room for header */
    if (!out) {
        free(buf);
        cJSON_AddStringToObject(result, "text",
            "error: memory allocation failed");
        return result;
    }

    /* Emit a header showing the range */
    int total_lines = 0;
    {
        /* First pass: count total lines */
        for (size_t i = 0; i < nread; i++) {
            if (buf[i] == '\n') total_lines++;
        }
        if (nread > 0 && buf[nread-1] != '\n') total_lines++;
    }

    /* Line-by-line scan */
    size_t line_start = 0;
    int current_line = 0;
    int lines_output = 0;

    for (size_t i = 0; i <= nread; i++) {
        if (i == nread || buf[i] == '\n') {
            current_line++;

            if (current_line >= offset && lines_output < limit) {
                /* Emit this line with line number prefix */
                size_t raw_len = (i == nread) ? (i - line_start) : (i - line_start);
                size_t line_len = raw_len;
                int truncated = 0;
                if (line_len > 500) {
                    line_len = 500;
                    truncated = 1;
                }

                /* 32 extra bytes for line number prefix + newline + [truncated] */
                if (collected + line_len + 32 > cap + 255) {
                    /* Would overflow — stop */
                    break;
                }

                if (collected > 0) {
                    out[collected++] = '\n';
                }

                /* Line number prefix, e.g. "     1  " */
                int num_len = snprintf(out + collected, 32, "%6d  ", current_line);
                collected += num_len;

                memcpy(out + collected, buf + line_start, line_len);
                collected += line_len;

                if (truncated) {
                    memcpy(out + collected, "[truncated]", 11);
                    collected += 11;
                }

                lines_output++;
            }

            line_start = i + 1;

            if (lines_output >= limit)
                break;
        }
    }

    out[collected] = '\0';

    cJSON_AddStringToObject(result, "text", out);
    free(out);
    free(buf);
    return result;
}

/* ============================================================================
 * Write Tool
 * ============================================================================ */

/* Resolve path to absolute. For existing files uses realpath();
 * for new paths prepends CWD. Caller must free the result. */
static char *resolve_abs_path(const char *path) {
    /* Try realpath first (works if file exists) */
    char *resolved = realpath(path, NULL);
    if (resolved) return resolved;

    /* If file doesn't exist, prepend CWD */
    if (path[0] == '/') {
        return strdup(path);
    }

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return strdup(path);

    size_t len = strlen(cwd) + 1 + strlen(path) + 1;
    char *abs = malloc(len);
    if (!abs) return strdup(path);
    snprintf(abs, len, "%s/%s", cwd, path);
    return abs;
}

cJSON *tool_write(llm_runtime_t *rt, const cJSON *args) {
    (void)rt;

    cJSON *p = cJSON_GetObjectItem(args, "path");
    const char *path = (p && cJSON_IsString(p)) ? p->valuestring : NULL;

    cJSON *c = cJSON_GetObjectItem(args, "content");
    const char *content = (c && cJSON_IsString(c)) ? c->valuestring : NULL;

    /* Parse mode: "overwrite", "append", or NULL */
    cJSON *m = cJSON_GetObjectItem(args, "mode");
    const char *mode = (m && cJSON_IsString(m)) ? m->valuestring : NULL;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "text");

    if (!path || !*path) {
        cJSON_AddStringToObject(result, "text",
            "Error: missing 'path' argument");
        return result;
    }
    if (!content) {
        char errbuf[512];
        char *abs = resolve_abs_path(path);
        snprintf(errbuf, sizeof(errbuf),
                 "Error writing to file '%s' (resolved from '%s'): missing 'content' argument",
                 abs, path);
        free(abs);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Resolve absolute path */
    char *abs_path = resolve_abs_path(path);
    size_t content_len = strlen(content);

    /* Check mode logic */
    if (mode == NULL) {
        /* mode not specified: refuse if file exists */
        FILE *check = fopen(abs_path, "rb");
        if (check) {
            fclose(check);
            char errbuf[1024];
            snprintf(errbuf, sizeof(errbuf),
                "Error: File already exists: %s. "
                "Use mode='overwrite' to overwrite, or mode='append' to append.",
                abs_path);
            free(abs_path);
            cJSON_AddStringToObject(result, "text", errbuf);
            return result;
        }
        mode = "overwrite";  /* default for new files */
    }

    if (strcmp(mode, "overwrite") != 0 && strcmp(mode, "append") != 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error: Invalid mode '%s'. Must be 'overwrite', 'append', or None.",
            mode);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Show preview */
    printf("\n  [tool] %s to: %s (%zu bytes)\n",
           strcmp(mode, "append") == 0 ? "append" : "write",
           abs_path, content_len);
    printf("\n  [preview first 600 chars]\n");
    size_t preview_len = content_len < 500 ? content_len : 500;
    printf("%.*s", (int)preview_len, content);
    if (content_len > preview_len)
        printf("\n  ... [%zu more bytes]", content_len - preview_len);
    printf("\n");

    if (!ask_approval("[yellow][b]Apply this file operation? y/N[/] ")) {
        free(abs_path);
        cJSON_AddStringToObject(result, "text",
            "user denied file write");
        return result;
    }

    /* Create parent directories */
    mkdir_p(abs_path);

    const char *fmode = (strcmp(mode, "append") == 0) ? "ab" : "wb";
    FILE *f = fopen(abs_path, fmode);
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error writing to file '%s' (resolved from '%s'): %s",
            abs_path, path, strerror(errno));
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);

    char summary[512];
    if (strcmp(mode, "append") == 0) {
        snprintf(summary, sizeof(summary),
            "Successfully appended to %s", abs_path);
    } else {
        snprintf(summary, sizeof(summary),
            "Successfully wrote to %s", abs_path);
    }
    printf("  [tool] %s\n", summary);
    free(abs_path);
    cJSON_AddStringToObject(result, "text", summary);
    return result;
}

/* ============================================================================
 * Edit Tool
 * ============================================================================ */

cJSON *tool_edit(llm_runtime_t *rt, const cJSON *args) {
    (void)rt;

    cJSON *p = cJSON_GetObjectItem(args, "path");
    const char *path = (p && cJSON_IsString(p)) ? p->valuestring : NULL;

    cJSON *o = cJSON_GetObjectItem(args, "old");
    const char *old_str = (o && cJSON_IsString(o)) ? o->valuestring : NULL;

    cJSON *n = cJSON_GetObjectItem(args, "new");
    const char *new_str = (n && cJSON_IsString(n)) ? n->valuestring : NULL;

    /* Parse replace_all (default false) */
    cJSON *ra = cJSON_GetObjectItem(args, "replace_all");
    int replace_all = (ra && cJSON_IsBool(ra)) ? cJSON_IsTrue(ra) : 0;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "text");

    if (!path || !*path) {
        cJSON_AddStringToObject(result, "text",
            "Error: missing 'path' argument");
        return result;
    }
    if (!old_str) {
        cJSON_AddStringToObject(result, "text",
            "Error: missing 'old' argument");
        return result;
    }
    if (!new_str) {
        cJSON_AddStringToObject(result, "text",
            "Error: missing 'new' argument");
        return result;
    }

    /* Resolve absolute path */
    char *abs_path = resolve_abs_path(path);

    /* Check file existence */
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error: File not found: %s (resolved from '%s')",
            abs_path, path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Check it's a regular file */
    if (!S_ISREG(st.st_mode)) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error: Path is not a file: %s (resolved from '%s')",
            abs_path, path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Read the file */
    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error processing file '%s' (resolved from '%s'): %s",
            abs_path, path, strerror(errno));
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0 || file_size > TOOL_READ_MAX_BYTES) {
        fclose(f);
        free(abs_path);
        cJSON_AddStringToObject(result, "text",
            file_size < 0
                ? "Error: cannot determine file size"
                : "Error: file too large to edit");
        return result;
    }

    char *buf = malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(f);
        free(abs_path);
        cJSON_AddStringToObject(result, "text",
            "Error: memory allocation failed");
        return result;
    }

    size_t nread = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    buf[nread] = '\0';

    /* Find the first match for preview */
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    char *first_match = strstr(buf, old_str);

    if (!first_match) {
        free(buf);
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "No occurrences found to replace in %s", abs_path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Show preview of the change (first occurrence) */
    size_t match_offset = (size_t)(first_match - buf);
    printf("\n  [tool] editing: %s\n", abs_path);
    printf("  [%s]\n", replace_all ? "replacing ALL occurrences" : "replacing first occurrence only");
    printf("  [context around 'old' (offset %zu)]\n", match_offset);

    size_t ctx_start = (match_offset > 60) ? match_offset - 60 : 0;
    printf("  ---\n");
    printf("%.*s", (int)(match_offset - ctx_start), buf + ctx_start);
    printf("\033[31m%s\033[0m", old_str);
    printf("%.*s", (int)((match_offset + old_len + 60 > nread)
                        ? nread - (match_offset + old_len)
                        : 60),
           buf + match_offset + old_len);
    printf("\n  ---\n");
    printf("  replacement:\n");
    printf("\033[32m%s\033[0m\n", new_str);
    printf("  ---\n");

    if (!ask_approval("[yellow][b]Apply this edit? y/N[/] ")) {
        free(buf);
        free(abs_path);
        cJSON_AddStringToObject(result, "text",
            "user denied file edit");
        return result;
    }

    /* Perform the replacement */
    char *new_content = NULL;
    size_t new_size = 0;

    if (replace_all) {
        /* Count occurrences to estimate output size */
        size_t count = 0;
        char *p = buf;
        while ((p = strstr(p, old_str)) != NULL) {
            count++;
            p += old_len;
        }

        /* Allocate worst-case: each replacement could be larger */
        new_size = nread + count * (new_len > old_len ? new_len - old_len : 0);
        new_content = malloc(new_size + 1);
        if (!new_content) {
            free(buf);
            free(abs_path);
            cJSON_AddStringToObject(result, "text",
                "Error: memory allocation failed");
            return result;
        }

        /* Build result by scanning and replacing */
        size_t pos = 0;
        char *src = buf;
        char *match;
        while ((match = strstr(src, old_str)) != NULL) {
            size_t before = (size_t)(match - src);
            memcpy(new_content + pos, src, before);
            pos += before;
            memcpy(new_content + pos, new_str, new_len);
            pos += new_len;
            src = match + old_len;
        }
        /* Copy remaining */
        size_t remaining = nread - (size_t)(src - buf);
        memcpy(new_content + pos, src, remaining);
        pos += remaining;
        new_content[pos] = '\0';
        new_size = pos;
    } else {
        /* Replace first occurrence only */
        size_t before_len = (size_t)(first_match - buf);
        size_t after_len  = nread - before_len - old_len;

        new_size = before_len + new_len + after_len;
        new_content = malloc(new_size + 1);
        if (!new_content) {
            free(buf);
            free(abs_path);
            cJSON_AddStringToObject(result, "text",
                "Error: memory allocation failed");
            return result;
        }

        memcpy(new_content, buf, before_len);
        memcpy(new_content + before_len, new_str, new_len);
        memcpy(new_content + before_len + new_len, first_match + old_len, after_len);
        new_content[new_size] = '\0';
    }

    /* Only write if changes were actually made */
    if (new_size == nread && memcmp(new_content, buf, nread) == 0) {
        free(new_content);
        free(buf);
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "No occurrences found to replace in %s", abs_path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    /* Write the edited file */
    FILE *out = fopen(abs_path, "wb");
    if (!out) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
            "Error processing file '%s' (resolved from '%s'): %s",
            abs_path, path, strerror(errno));
        free(new_content);
        free(buf);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", errbuf);
        return result;
    }

    fwrite(new_content, 1, new_size, out);
    fclose(out);

    char summary[512];
    snprintf(summary, sizeof(summary),
        "Successfully replaced in %s", abs_path);
    printf("  [tool] %s\n", summary);

    cJSON_AddStringToObject(result, "text", summary);
    free(new_content);
    free(buf);
    free(abs_path);
    return result;
}

/* ============================================================================
 * Tool Schema Helpers
 * ============================================================================ */

void tool_functions_add_sleep_schema(cJSON *schemas) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");

    cJSON *func = cJSON_AddObjectToObject(schema, "function");
    cJSON_AddStringToObject(func, "name", "sleep");
    cJSON_AddStringToObject(func, "description",
        "Sleep for a given number of seconds");

    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *secs = cJSON_AddObjectToObject(props, "secs");
    cJSON_AddStringToObject(secs, "type", "number");
    cJSON_AddStringToObject(secs, "description",
        "Number of seconds to sleep");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("secs"));

    cJSON_AddItemToArray(schemas, schema);
}

void tool_functions_add_shell_schema(cJSON *schemas) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");

    cJSON *func = cJSON_AddObjectToObject(schema, "function");
    cJSON_AddStringToObject(func, "name", "shell");
    cJSON_AddStringToObject(func, "description",
        "Run a shell command and return its output (stdout+stderr merged). "
        "Useful for: ls, cat, grep, find, wc, date, curl, git status, etc. "
        "Avoid commands that run forever or require interactive input.");

    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *cmd = cJSON_AddObjectToObject(props, "cmd");
    cJSON_AddStringToObject(cmd, "type", "string");
    cJSON_AddStringToObject(cmd, "description",
        "The shell command to execute");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("cmd"));

    cJSON_AddItemToArray(schemas, schema);
}

void tool_functions_add_read_schema(cJSON *schemas) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");

    cJSON *func = cJSON_AddObjectToObject(schema, "function");
    cJSON_AddStringToObject(func, "name", "read");
    cJSON_AddStringToObject(func, "description",
        "Read the contents of a file. Use this to view source code, "
        "configuration files, logs, or any text file. "
        "Maximum returned size is 100 KB. No approval needed.");

    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *path = cJSON_AddObjectToObject(props, "path");
    cJSON_AddStringToObject(path, "type", "string");
    cJSON_AddStringToObject(path, "description",
        "Path to the file to read");

    cJSON *off = cJSON_AddObjectToObject(props, "offset");
    cJSON_AddStringToObject(off, "type", "integer");
    cJSON_AddStringToObject(off, "description",
        "Starting line number (1-indexed). Default: 1");

    cJSON *lim = cJSON_AddObjectToObject(props, "limit");
    cJSON_AddStringToObject(lim, "type", "integer");
    cJSON_AddStringToObject(lim, "description",
        "Maximum number of lines to return (max 1000). Default: 1000");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("path"));

    cJSON_AddItemToArray(schemas, schema);
}

void tool_functions_add_write_schema(cJSON *schemas) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");

    cJSON *func = cJSON_AddObjectToObject(schema, "function");
    cJSON_AddStringToObject(func, "name", "write");
    cJSON_AddStringToObject(func, "description",
        "Write or append content to a file. Parent directories are created "
        "automatically. If mode is not specified and the file already exists, "
        "returns an error asking for an explicit mode. "
        "Requires user approval before writing.");

    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *path = cJSON_AddObjectToObject(props, "path");
    cJSON_AddStringToObject(path, "type", "string");
    cJSON_AddStringToObject(path, "description",
        "Absolute or relative path to the file");

    cJSON *content = cJSON_AddObjectToObject(props, "content");
    cJSON_AddStringToObject(content, "type", "string");
    cJSON_AddStringToObject(content, "description",
        "String content to write");

    cJSON *mode = cJSON_AddObjectToObject(props, "mode");
    cJSON_AddStringToObject(mode, "type", "string");
    cJSON_AddStringToObject(mode, "description",
        "Write mode: 'overwrite' to overwrite, 'append' to append. "
        "If absent and file exists, returns an error.");
    cJSON *enum_arr = cJSON_CreateArray();
    cJSON_AddItemToArray(enum_arr, cJSON_CreateString("overwrite"));
    cJSON_AddItemToArray(enum_arr, cJSON_CreateString("append"));
    cJSON_AddItemToObject(mode, "enum", enum_arr);

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("path"));
    cJSON_AddItemToArray(required, cJSON_CreateString("content"));

    cJSON_AddItemToArray(schemas, schema);
}

void tool_functions_add_edit_schema(cJSON *schemas) {
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");

    cJSON *func = cJSON_AddObjectToObject(schema, "function");
    cJSON_AddStringToObject(func, "name", "edit");
    cJSON_AddStringToObject(func, "description",
        "Replace substring(s) in a file. Reads the file, finds matches "
        "of 'old', replaces with 'new'. By default replaces only the first "
        "occurrence; set replace_all=true to replace all. "
        "Shows a diff preview and requires user approval before applying.");

    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *path = cJSON_AddObjectToObject(props, "path");
    cJSON_AddStringToObject(path, "type", "string");
    cJSON_AddStringToObject(path, "description",
        "Absolute or relative path to the file");

    cJSON *old_p = cJSON_AddObjectToObject(props, "old");
    cJSON_AddStringToObject(old_p, "type", "string");
    cJSON_AddStringToObject(old_p, "description",
        "Substring to replace");

    cJSON *new_p = cJSON_AddObjectToObject(props, "new");
    cJSON_AddStringToObject(new_p, "type", "string");
    cJSON_AddStringToObject(new_p, "description",
        "Replacement substring");

    cJSON *ra = cJSON_AddObjectToObject(props, "replace_all");
    cJSON_AddStringToObject(ra, "type", "boolean");
    cJSON_AddStringToObject(ra, "description",
        "If true, replace all occurrences; otherwise replace only the first");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("path"));
    cJSON_AddItemToArray(required, cJSON_CreateString("old"));
    cJSON_AddItemToArray(required, cJSON_CreateString("new"));

    cJSON_AddItemToArray(schemas, schema);
}

cJSON *tool_functions_create_schema(void) {
    cJSON *schemas = cJSON_CreateArray();
    if (!schemas) return NULL;

    tool_functions_add_sleep_schema(schemas);
    tool_functions_add_shell_schema(schemas);
    tool_functions_add_read_schema(schemas);
    tool_functions_add_write_schema(schemas);
    tool_functions_add_edit_schema(schemas);
    return schemas;
}
