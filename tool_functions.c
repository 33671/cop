/*
 * tool_functions.c
 *
 * Tool function implementations and schema helpers for the LLM runtime.
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
#include "sanitize_utf8.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define TOOL_READ_MAX_BYTES  (100 * 1024)   /* max file size for read/edit */
#define OUTPUT_MAX_LINE      1000           /* per-line char limit           */
#define OUTPUT_MAX_TOTAL     8000           /* overall char limit            */

/* ============================================================================
 * Shared Helpers — cJSON result construction
 * ============================================================================ */

/* Create a standard {"type":"text", "text":<msg>} result object.
 * Never returns NULL — OOM is fatal and cannot be recovered from
 * at the cJSON level, so we abort cleanly. */
static cJSON *new_text_result(const char *msg) {
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        fprintf(stderr, "FATAL: cJSON_CreateObject() out of memory\n");
        abort();
    }
    cJSON_AddStringToObject(r, "type", "text");
    if (msg) cJSON_AddStringToObject(r, "text", msg);
    return r;
}

/* ============================================================================
 * Shared Helpers — user approval
 * ============================================================================ */

/* Ask y/N. Returns 1 approved, 0 denied, -1 Ctrl+C.
   Empty/whitespace → approve; only 'n'/'N' → deny.
   In --yolo mode, skips prompt and always returns 1. */
static int ask_approval(llm_runtime_t *rt, const char *prompt) {
    (void)rt;
    if (llm_runtime_is_yolo(rt)) return 1;
    ic_enable_multiline(false);  /* singleline: Enter → submit */
    char *reply = ic_readline(prompt);
    ic_enable_multiline(true);   /* restore */
    if (ic_was_interrupted()) { free(reply); return -1; }
    if (!reply)               return -1;
    size_t rlen = strlen(reply);
    while (rlen > 0 && (reply[rlen-1] == '\n' || reply[rlen-1] == '\r'))
        reply[--rlen] = '\0';
    /* Only 'n'/'N' denies; everything else (empty, spaces, 'y', etc.) approves */
    int denied = 0;
    for (size_t i = 0; i < rlen; i++) {
        if (reply[i] == 'n' || reply[i] == 'N') {
            denied = 1;
            break;
        }
        if (reply[i] != ' ' && reply[i] != '\t') break; /* stop at first non-whitespace */
    }
    free(reply);
    return denied ? 0 : 1;
}

/* Full approval flow: ask prompt, then either cancel runtime, deny with
 * deny_msg, or proceed.  Sets result->text and returns 0 when the caller
 * should return result immediately.  Returns 1 when approved. */
static int check_approval(llm_runtime_t *rt, cJSON *result,
                           const char *prompt, const char *deny_msg) {
    int a = ask_approval(rt, prompt);
    if (a < 0) {
        llm_runtime_cancel(rt);
        cJSON_AddStringToObject(result, "text", "cancelled");
        return 0;
    }
    if (a == 0) {
        llm_runtime_cancel(rt);
        cJSON_AddStringToObject(result, "text", deny_msg);
        return 0;
    }
    return 1;
}

/* ============================================================================
 * Shared Helpers — path handling
 * ============================================================================ */

/* Expand leading ~ to $HOME. Caller frees the result. */
static char *expand_tilde(const char *path) {
    if (!path || path[0] != '~') return strdup(path);
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t len = strlen(home) + strlen(path);
    char *r = malloc(len);
    if (!r) return strdup(path);
    snprintf(r, len, "%s%s", home, path + 1);
    return r;
}

/* Resolve path to absolute: ~ expansion, realpath, CWD fallback.
 * Caller frees the result. */
static char *resolve_abs_path(const char *path) {
    char *expanded = expand_tilde(path);
    char *resolved = realpath(expanded, NULL);
    if (resolved) { free(expanded); return resolved; }
    if (expanded[0] == '/') return expanded;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return expanded;
    size_t len = strlen(cwd) + 1 + strlen(expanded) + 1;
    char *abs = malloc(len);
    if (!abs) return expanded;
    snprintf(abs, len, "%s/%s", cwd, expanded);
    free(expanded);
    return abs;
}

/* Extract "path" string arg.  If missing/empty, sets result->text and
 * returns NULL (caller should return result). */
static const char *get_path_arg(const cJSON *args, cJSON *result) {
    cJSON *p = cJSON_GetObjectItem(args, "path");
    const char *path = (p && cJSON_IsString(p)) ? p->valuestring : NULL;
    if (!path || !*path)
        cJSON_AddStringToObject(result, "text",
            "error: missing 'path' argument");
    return (path && *path) ? path : NULL;
}

/* Create parent directories for a given fs path. */
static int mkdir_p(const char *path) {
    if (strlen(path) >= 1024) return -1;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    return 0;
}

/* ============================================================================
 * Shared Helpers — file I/O
 * ============================================================================ */

/* Read file into a heap buffer.  Uses stat() (not fseek/ftell) so it
 * works correctly with pipes, /proc files, and other special files.
 * On error sets result->text and returns NULL.
 * On success *out_len receives bytes read.  Caller frees the buffer. */
static char *read_file_at(const char *abs_path, size_t max_bytes,
                           size_t *out_len, cJSON *result) {
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        char eb[256];
        snprintf(eb, sizeof(eb), "error: cannot stat file: %s",
                 strerror(errno));
        cJSON_AddStringToObject(result, "text", eb);
        return NULL;
    }
    /* Only use st_size for regular files; fall back to reading
       up to max_bytes for pipes, devices, /proc entries, etc. */
    size_t n = S_ISREG(st.st_mode) && st.st_size >= 0
               ? ((size_t)st.st_size < max_bytes ? (size_t)st.st_size : max_bytes)
               : max_bytes;

    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        char eb[256];
        snprintf(eb, sizeof(eb), "error: cannot open file: %s",
                 strerror(errno));
        cJSON_AddStringToObject(result, "text", eb);
        return NULL;
    }
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    *out_len = fread(buf, 1, n, f);
    fclose(f);
    buf[*out_len] = '\0';
    return buf;
}

/* ============================================================================
 * Shared Helpers — line-based diff
 * ============================================================================ */

/*
 * Line-based diff: compares old and new content and produces a compact
 * unified-diff-style output.  Only changed regions (with 3 lines of
 * context) are included.
 *
 * On success, *diff_out is set to a malloc'd string (caller frees) and
 * the function returns its length.  On failure returns -1.
 */
static int generate_diff(const char *old, size_t old_len,
                          const char *new, size_t new_len,
                          char **diff_out) {
    *diff_out = NULL;
    if (!old || !new) return -1;

    /* Split old into lines */
    size_t   old_cap = 64;
    size_t   old_cnt = 0;
    char   **old_lines = malloc(old_cap * sizeof(char *));
    if (!old_lines) return -1;

    const char *p = old;
    const char *eof = old + old_len;
    while (p < eof) {
        const char *eol = memchr(p, '\n', eof - p);
        size_t len = eol ? (size_t)(eol - p) : (size_t)(eof - p);
        char *line = malloc(len + 1);
        if (!line) goto cleanup_old;
        memcpy(line, p, len);
        line[len] = '\0';
        if (old_cnt >= old_cap) {
            old_cap *= 2;
            char **tmp = realloc(old_lines, old_cap * sizeof(char *));
            if (!tmp) { free(line); goto cleanup_old; }
            old_lines = tmp;
        }
        old_lines[old_cnt++] = line;
        p = eol ? eol + 1 : eof;
    }

    /* Split new into lines */
    size_t   new_cap = 64;
    size_t   new_cnt = 0;
    char   **new_lines = malloc(new_cap * sizeof(char *));
    if (!new_lines) goto cleanup_old;

    p = new;
    eof = new + new_len;
    while (p < eof) {
        const char *eol = memchr(p, '\n', eof - p);
        size_t len = eol ? (size_t)(eol - p) : (size_t)(eof - p);
        char *line = malloc(len + 1);
        if (!line) goto cleanup_new;
        memcpy(line, p, len);
        line[len] = '\0';
        if (new_cnt >= new_cap) {
            new_cap *= 2;
            char **tmp = realloc(new_lines, new_cap * sizeof(char *));
            if (!tmp) { free(line); goto cleanup_new; }
            new_lines = tmp;
        }
        new_lines[new_cnt++] = line;
        p = eol ? eol + 1 : eof;
    }

    /* Build output: walk both arrays, find hunks, emit diff */
    size_t out_cap = 4096;
    size_t out_len = 0;
    char  *out = malloc(out_cap);
    if (!out) goto cleanup_new;
    out[0] = '\0';

    size_t oi = 0, ni = 0;  /* current position in old/new lines */

    while (oi < old_cnt || ni < new_cnt) {
        /* Skip common prefix */
        size_t common_start = 0;
        while (oi < old_cnt && ni < new_cnt &&
               strcmp(old_lines[oi], new_lines[ni]) == 0) {
            oi++; ni++; common_start++;
        }
        if (common_start == 0 && oi >= old_cnt && ni >= new_cnt) break;
        if (oi >= old_cnt && ni >= new_cnt) break;

        /* Find the changed hunk */
        size_t hunk_old_start = oi;
        size_t hunk_new_start = ni;

        /* Find where old and new re-sync after the change */
        size_t sync_old = old_cnt;
        size_t sync_new = new_cnt;
        for (size_t o = oi; o < old_cnt; o++) {
            for (size_t n = ni; n < new_cnt; n++) {
                if (strcmp(old_lines[o], new_lines[n]) == 0) {
                    /* Check if this is a plausible re-sync point */
                    size_t match = 0;
                    size_t mo = o, mn = n;
                    while (mo < old_cnt && mn < new_cnt &&
                           strcmp(old_lines[mo], new_lines[mn]) == 0 &&
                           match < 3) {
                        mo++; mn++; match++;
                    }
                    if (match >= 2 || (o == oi && n == ni)) {
                        sync_old = o;
                        sync_new = n;
                        goto found_sync;
                    }
                }
            }
        }
found_sync:

        /* Compute context: show 3 lines before and after */
        size_t ctx_before = (hunk_old_start >= 3) ? 3 : hunk_old_start;
        size_t hunk_old_end = sync_old;
        size_t hunk_new_end = sync_new;
        size_t ctx_after  = 0;
        /* Look ahead for more changes near the sync point */
        size_t look_old = sync_old, look_new = sync_new;
        while (look_old < old_cnt && look_new < new_cnt &&
               strcmp(old_lines[look_old], new_lines[look_new]) == 0 &&
               ctx_after < 3) {
            look_old++; look_new++; ctx_after++;
        }

        /* Emit hunk header */
        size_t old_display_start = hunk_old_start - ctx_before + 1;  /* 1-indexed */
        size_t old_display_count = (hunk_old_end - hunk_old_start) + ctx_before + ctx_after;
        size_t new_display_start = hunk_new_start - ctx_before + 1;  /* 1-indexed */
        size_t new_display_count = (hunk_new_end - hunk_new_start) + ctx_before + ctx_after;

        char header[128];
        int hdr_len = snprintf(header, sizeof(header),
                               "@@ -%zu,%zu +%zu,%zu @@\n",
                               old_display_start, old_display_count,
                               new_display_start, new_display_count);
        if (out_len + (size_t)hdr_len + 1 >= out_cap) {
            out_cap = (out_cap * 2 > out_len + hdr_len + 1)
                      ? out_cap * 2 : out_len + hdr_len + 256;
            char *tmp = realloc(out, out_cap);
            if (!tmp) { free(out); goto cleanup_new; }
            out = tmp;
        }
        memcpy(out + out_len, header, hdr_len);
        out_len += hdr_len;

        /* Emit context before */
        for (size_t i = hunk_old_start - ctx_before; i < hunk_old_start; i++) {
            if (i >= old_cnt) break;
            size_t need = out_len + strlen(old_lines[i]) + 3;
            if (need >= out_cap) {
                out_cap = out_cap * 2;
                if (out_cap < need) out_cap = need + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); goto cleanup_new; }
                out = tmp;
            }
            out[out_len++] = ' ';
            size_t ln = strlen(old_lines[i]);
            memcpy(out + out_len, old_lines[i], ln);
            out_len += ln;
            out[out_len++] = '\n';
        }

        /* Emit removed lines */
        for (size_t i = hunk_old_start; i < hunk_old_end; i++) {
            if (i >= old_cnt) break;
            size_t need = out_len + strlen(old_lines[i]) + 3;
            if (need >= out_cap) {
                out_cap = out_cap * 2;
                if (out_cap < need) out_cap = need + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); goto cleanup_new; }
                out = tmp;
            }
            out[out_len++] = '-';
            size_t ln = strlen(old_lines[i]);
            memcpy(out + out_len, old_lines[i], ln);
            out_len += ln;
            out[out_len++] = '\n';
        }

        /* Emit added lines */
        for (size_t i = hunk_new_start; i < hunk_new_end; i++) {
            if (i >= new_cnt) break;
            size_t need = out_len + strlen(new_lines[i]) + 3;
            if (need >= out_cap) {
                out_cap = out_cap * 2;
                if (out_cap < need) out_cap = need + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); goto cleanup_new; }
                out = tmp;
            }
            out[out_len++] = '+';
            size_t ln = strlen(new_lines[i]);
            memcpy(out + out_len, new_lines[i], ln);
            out_len += ln;
            out[out_len++] = '\n';
        }

        /* Emit context after */
        for (size_t i = hunk_old_end; i < hunk_old_end + ctx_after && i < old_cnt; i++) {
            size_t need = out_len + strlen(old_lines[i]) + 3;
            if (need >= out_cap) {
                out_cap = out_cap * 2;
                if (out_cap < need) out_cap = need + 256;
                char *tmp = realloc(out, out_cap);
                if (!tmp) { free(out); goto cleanup_new; }
                out = tmp;
            }
            out[out_len++] = ' ';
            size_t ln = strlen(old_lines[i]);
            memcpy(out + out_len, old_lines[i], ln);
            out_len += ln;
            out[out_len++] = '\n';
        }

        /* Advance past the hunk */
        oi = hunk_old_end + ctx_after;
        ni = hunk_new_end + ctx_after;
    }

    out[out_len] = '\0';

    /* Cleanup lines */
    for (size_t i = 0; i < old_cnt; i++) free(old_lines[i]);
    for (size_t i = 0; i < new_cnt; i++) free(new_lines[i]);
    free(old_lines);
    free(new_lines);

    *diff_out = out;
    return (int)out_len;

cleanup_new:
    for (size_t i = 0; i < new_cnt; i++) free(new_lines[i]);
    free(new_lines);
cleanup_old:
    for (size_t i = 0; i < old_cnt; i++) free(old_lines[i]);
    free(old_lines);
    return -1;
}

/* ============================================================================
 * Shared Helpers — output sanitization
 * ============================================================================ */

/* In-place sanitize + per-line truncation + overall truncation.
 * Takes ownership of *out_p and replaces it with a clean malloc'd copy.
 * max_line / max_total are the per-line and overall byte limits. */
static void sanitize_truncate_output(char **out_p,
                                      int max_line, int max_total) {
    char *raw = *out_p;
    if (!raw || !*raw) return;

    /* Step 1 — UTF-8 sanitize in-place */
    sanitize_utf8((uint8_t *)raw, strlen(raw));

    /* Allocate enough for max_total output + truncation markers.
       raw_len may be much smaller than max_total (e.g. short input
       with many tiny lines that get expanded by [truncated] tags). */
    char *clean = malloc((size_t)max_total + 256);
    if (!clean) return;
    size_t raw_len = strlen(raw);

    size_t written = 0;
    char *p   = raw;
    int   done = 0;

    while (*p && !done) {
        char  *eol     = strchr(p, '\n');
        size_t raw_ln  = eol ? (size_t)(eol - p) : strlen(p);
        int    trunc   = 0;
        size_t keep    = raw_ln;
        if (keep > (size_t)max_line) { keep = max_line; trunc = 1; }

        size_t need = keep + (trunc ? 11 : 0) + 1;
        if (written + need > (size_t)max_total) {
            written += (size_t)snprintf(clean + written,
                        (size_t)max_total + 256 - written,
                        "... [truncated at %zu chars]\n", written);
            done = 1;
            break;
        }
        memcpy(clean + written, p, keep);  written += keep;
        if (trunc) {
            memcpy(clean + written, "[truncated]", 11);
            written += 11;
        }
        clean[written++] = '\n';
        if (!eol) break;
        p = eol + 1;
    }
    clean[written] = '\0';
    free(raw);
    *out_p = clean;
}

/* ============================================================================
 * Sleep Tool
 * ============================================================================ */

cJSON *tool_sleep(llm_runtime_t *rt, const cJSON *args) {
    double secs = 1.0;
    cJSON *s = cJSON_GetObjectItem(args, "secs");
    if (s && cJSON_IsNumber(s)) secs = s->valuedouble;

    printf("\n  [tool] sleeping for %.1f seconds...\n", secs);

    const double step = 0.1;
    double slept = 0;
    while (slept < secs) {
        if (llm_runtime_is_cancelled(rt)) {
            printf("  [tool] cancelled!\n");
            return new_text_result("cancelled");
        }
        usleep((useconds_t)(step * 1000000));
        slept += step;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "done, slept for %.1f seconds", secs);
    return new_text_result(buf);
}

/* ============================================================================
 * Shell Tool
 * ============================================================================ */

cJSON *tool_shell(llm_runtime_t *rt, const cJSON *args) {
    cJSON *cmd_json = cJSON_GetObjectItem(args, "cmd");
    const char *cmd = (cmd_json && cJSON_IsString(cmd_json))
                      ? cmd_json->valuestring : NULL;

    cJSON *result = new_text_result(NULL);
    if (!cmd || !*cmd) {
        cJSON_AddStringToObject(result, "text",
            "error: missing 'cmd' argument");
        return result;
    }

    printf("\n  [tool] proposed command: %s\n", cmd);
    if (!check_approval(rt, result,
            "[yellow][b]Run this command? y/N[/] ",
            "user denied shell execution"))
        return result;

    printf("  [tool] running: %s\n", cmd);

    char *output = NULL;
    int exit_code = 0;
    int ret = llm_runtime_popen(rt, cmd, now() + 7200000, &output, &exit_code);

    sanitize_truncate_output(&output, OUTPUT_MAX_LINE, OUTPUT_MAX_TOTAL);

    /* Build text field */
    size_t text_len = 128 + (output ? strlen(output) : 0);
    char *text_buf = malloc(text_len);
    if (ret != 0) {
        const char *reason = llm_runtime_is_cancelled(rt)
                             ? "cancelled" : "timed out";
        snprintf(text_buf, text_len,
                 "Output:\n%s\nExit_code:%d\n"
                 "[WARNING: command %s partial output above]",
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
    cJSON *result = new_text_result(NULL);

    const char *path = get_path_arg(args, result);
    if (!path) return result;

    /* Parse offset (1-indexed, default 1), limit (default 1000, max 1000) */
    int offset = 1, limit = 1000;
    cJSON *off = cJSON_GetObjectItem(args, "offset");
    cJSON *lim = cJSON_GetObjectItem(args, "limit");
    if (off && cJSON_IsNumber(off)) { int v = off->valueint; offset = (v >= 1) ? v : 1; }
    if (lim && cJSON_IsNumber(lim)) {
        int v = lim->valueint;
        limit = (v >= 1) ? ((v <= 1000) ? v : 1000) : 1;
    }

    printf("\n  [tool] reading: %s (offset=%d, limit=%d)\n", path, offset, limit);

    char *abs_path = resolve_abs_path(path);
    size_t nread = 0;
    char *buf = read_file_at(abs_path, TOOL_READ_MAX_BYTES, &nread, result);
    free(abs_path);
    if (!buf) return result;

    sanitize_utf8((uint8_t *)buf, nread);

    /* Count total lines for header */
    int total_lines = 0;
    for (size_t i = 0; i < nread; i++)
        if (buf[i] == '\n') total_lines++;
    if (nread > 0 && buf[nread-1] != '\n') total_lines++;

    /* Line-by-line scan with per-line truncation.
       Output is built in a dynamic buffer because line-number prefixes
       and newlines add per-line overhead that can significantly exceed
       the raw file size (e.g. 2500-byte file × 113 lines = ~3500 bytes
       of output).  A fixed `nread + 256` buffer silently truncates. */
    size_t out_cap = nread + 4096;
    if (out_cap < 8192) out_cap = 8192;
    char  *out = malloc(out_cap);
    if (!out) { free(buf); cJSON_AddStringToObject(result, "text",
        "error: memory allocation failed"); return result; }

    size_t collected    = 0;
    size_t line_start   = 0;
    int    current_line = 0;
    int    lines_out    = 0;

    for (size_t i = 0; i <= nread && lines_out < limit; i++) {
        int is_eol = (i == nread || buf[i] == '\n');
        if (!is_eol) continue;

        current_line++;
        if (current_line < offset) { line_start = i + 1; continue; }

        size_t raw_len = (i == nread) ? (i - line_start) : (i - line_start);
        size_t line_len = raw_len;
        int    truncated = 0;
        if (line_len > OUTPUT_MAX_LINE) { line_len = OUTPUT_MAX_LINE; truncated = 1; }

        /* Grow output buffer if needed (line + prefix + newline + [truncated]) */
        size_t need = line_len + 32;
        if (collected + need >= out_cap) {
            out_cap = out_cap * 2;
            if (out_cap < collected + need) out_cap = collected + need + 4096;
            char *tmp = realloc(out, out_cap);
            if (!tmp) { free(out); free(buf);
                cJSON_AddStringToObject(result, "text",
                    "error: memory allocation failed"); return result; }
            out = tmp;
        }

        if (collected > 0) out[collected++] = '\n';
        collected += (size_t)snprintf(out + collected, 32, "%6d  ", current_line);
        memcpy(out + collected, buf + line_start, line_len);
        collected += line_len;
        if (truncated) {
            memcpy(out + collected, "[truncated]", 11);
            collected += 11;
        }
        lines_out++;
        line_start = i + 1;
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

cJSON *tool_write(llm_runtime_t *rt, const cJSON *args) {
    (void)rt;
    cJSON *result = new_text_result(NULL);

    const char *path = get_path_arg(args, result);
    if (!path) return result;

    cJSON *c     = cJSON_GetObjectItem(args, "content");
    cJSON *m     = cJSON_GetObjectItem(args, "mode");
    const char *content = (c && cJSON_IsString(c)) ? c->valuestring : NULL;
    const char *mode    = (m && cJSON_IsString(m)) ? m->valuestring : NULL;

    if (!content) {
        char *abs = resolve_abs_path(path);
        char eb[512];
        snprintf(eb, sizeof(eb),
            "Error writing to file '%s' (resolved from '%s'): "
            "missing 'content' argument", abs, path);
        free(abs);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }

    char  *abs_path    = resolve_abs_path(path);
    size_t content_len = strlen(content);

    /* Mode logic */
    if (!mode) {
        FILE *check = fopen(abs_path, "rb");
        if (check) {
            fclose(check);
            char eb[1024];
            snprintf(eb, sizeof(eb),
                "Error: File already exists: %s. "
                "Use mode='overwrite' to overwrite, or mode='append' to append.",
                abs_path);
            free(abs_path);
            cJSON_AddStringToObject(result, "text", eb);
            return result;
        }
        mode = "overwrite";
    }
    if (strcmp(mode, "overwrite") != 0 && strcmp(mode, "append") != 0) {
        char eb[512];
        snprintf(eb, sizeof(eb),
            "Error: Invalid mode '%s'. Must be 'overwrite', 'append', or None.",
            mode);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }

    /* Preview */
    printf("\n  [tool] %s to: %s (%zu bytes)\n",
           strcmp(mode, "append") == 0 ? "append" : "write",
           abs_path, content_len);
    printf("\n  [preview first 500 chars]\n");
    size_t pv = content_len < 500 ? content_len : 500;
    printf("%.*s", (int)pv, content);
    if (content_len > pv) printf("\n  ... [%zu more bytes]", content_len - pv);
    printf("\n");

    if (!check_approval(rt, result,
            "[yellow][b]Apply this file operation? y/N[/] ",
            "user denied file write"))
        { free(abs_path); return result; }

    mkdir_p(abs_path);

    const char *fmode = (strcmp(mode, "append") == 0) ? "ab" : "wb";
    FILE *f = fopen(abs_path, fmode);
    if (!f) {
        char eb[512];
        snprintf(eb, sizeof(eb),
            "Error writing to file '%s' (resolved from '%s'): %s",
            abs_path, path, strerror(errno));
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }
    size_t wrote = fwrite(content, 1, content_len, f);
    fclose(f);

    char summary[512];
    if (wrote != content_len) {
        snprintf(summary, sizeof(summary),
            "Error: wrote %zu/%zu bytes to %s (disk full?)",
            wrote, content_len, abs_path);
    } else if (strcmp(mode, "append") == 0) {
        snprintf(summary, sizeof(summary), "Successfully appended to %s", abs_path);
    } else {
        snprintf(summary, sizeof(summary), "Successfully wrote to %s", abs_path);
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
    cJSON *result = new_text_result(NULL);

    const char *path = get_path_arg(args, result);
    if (!path) return result;

    cJSON *o  = cJSON_GetObjectItem(args, "old");
    cJSON *n  = cJSON_GetObjectItem(args, "new");
    cJSON *ra = cJSON_GetObjectItem(args, "replace_all");
    const char *old_str     = (o  && cJSON_IsString(o))  ? o->valuestring  : NULL;
    const char *new_str     = (n  && cJSON_IsString(n))  ? n->valuestring  : NULL;
    int         replace_all = (ra && cJSON_IsBool(ra))   ? cJSON_IsTrue(ra) : 0;

    if (!old_str || !*old_str) {
        cJSON_AddStringToObject(result, "text",
            "Error: 'old' argument must be a non-empty string");
        return result;
    }
    if (!new_str) {
        cJSON_AddStringToObject(result, "text", "Error: missing 'new' argument");
        return result;
    }

    char *abs_path = resolve_abs_path(path);

    /* File existence & type */
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        char eb[512];
        snprintf(eb, sizeof(eb), "Error: File not found: %s (resolved from '%s')",
                 abs_path, path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }
    if (!S_ISREG(st.st_mode)) {
        char eb[512];
        snprintf(eb, sizeof(eb), "Error: Path is not a file: %s (resolved from '%s')",
                 abs_path, path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }

    /* Read file */
    size_t nread = 0;
    char *buf = read_file_at(abs_path, TOOL_READ_MAX_BYTES, &nread, result);
    if (!buf) { free(abs_path); return result; }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    char  *first_match = strstr(buf, old_str);

    if (!first_match) {
        free(buf);
        char eb[512];
        snprintf(eb, sizeof(eb), "No occurrences found to replace in %s", abs_path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }

    /* Perform replacement (compute new_content before preview for diff) */
    char  *new_content = NULL;
    size_t new_size    = 0;

    if (replace_all) {
        size_t count = 0;
        for (char *p = buf; (p = strstr(p, old_str)); p += old_len) count++;
        new_size    = nread + count * (new_len > old_len ? new_len - old_len : 0);
        new_content = malloc(new_size + 1);
        if (!new_content) { free(buf); free(abs_path);
            cJSON_AddStringToObject(result, "text",
                "error: memory allocation failed"); return result; }

        size_t pos = 0;
        char  *src = buf, *match;
        while ((match = strstr(src, old_str))) {
            size_t before = (size_t)(match - src);
            memcpy(new_content + pos, src, before);           pos += before;
            memcpy(new_content + pos, new_str, new_len);      pos += new_len;
            src = match + old_len;
        }
        size_t rem = nread - (size_t)(src - buf);
        memcpy(new_content + pos, src, rem);  pos += rem;
        new_content[pos] = '\0';
        new_size = pos;
    } else {
        size_t before_len = (size_t)(first_match - buf);
        size_t after_len  = nread - before_len - old_len;
        new_size    = before_len + new_len + after_len;
        new_content = malloc(new_size + 1);
        if (!new_content) { free(buf); free(abs_path);
            cJSON_AddStringToObject(result, "text",
                "error: memory allocation failed"); return result; }
        memcpy(new_content, buf, before_len);
        memcpy(new_content + before_len, new_str, new_len);
        memcpy(new_content + before_len + new_len,
               first_match + old_len, after_len);
        new_content[new_size] = '\0';
    }

    if (new_size == nread && memcmp(new_content, buf, nread) == 0) {
        free(new_content); free(buf);
        char eb[512];
        snprintf(eb, sizeof(eb), "No occurrences found to replace in %s", abs_path);
        free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }

    /* Generate diff for preview and result */
    char *diff_text = NULL;
    int diff_len = generate_diff(buf, nread, new_content, new_size, &diff_text);

    /* Preview: show file path, replace mode, and diff */
    printf("\n  [tool] editing: %s\n", abs_path);
    printf("  [%s]\n", replace_all
           ? "replacing ALL occurrences" : "replacing first occurrence only");
    if (diff_text && diff_len > 0) {
        printf("  [diff]\n");
        /* Print diff with color: - lines in red, + lines in green */
        const char *dp = diff_text;
        while (*dp) {
            const char *eol = strchr(dp, '\n');
            if (!eol) eol = dp + strlen(dp);
            if (*dp == '-') {
                printf("\033[31m%.*s\033[0m\n", (int)(eol - dp), dp);
            } else if (*dp == '+') {
                printf("\033[32m%.*s\033[0m\n", (int)(eol - dp), dp);
            } else if (*dp == '@') {
                printf("\033[36m%.*s\033[0m\n", (int)(eol - dp), dp);
            } else {
                printf("%.*s\n", (int)(eol - dp), dp);
            }
            dp = (*eol == '\n') ? eol + 1 : eol;
        }
    }

    if (!check_approval(rt, result,
            "[yellow][b]Apply this edit? y/N[/] ",
            "user denied file edit"))
        { free(new_content); free(buf); free(diff_text); free(abs_path); return result; }

    /* Write to file */
    FILE *out_f = fopen(abs_path, "wb");
    if (!out_f) {
        char eb[512];
        snprintf(eb, sizeof(eb),
            "Error processing file '%s' (resolved from '%s'): %s",
            abs_path, path, strerror(errno));
        free(new_content); free(buf); free(diff_text); free(abs_path);
        cJSON_AddStringToObject(result, "text", eb);
        return result;
    }
    size_t wrote = fwrite(new_content, 1, new_size, out_f);
    fclose(out_f);

    /* Build result: short summary + diff */
    char summary[1024];
    if (wrote != new_size) {
        snprintf(summary, sizeof(summary),
            "Error: wrote %zu/%zu bytes to %s (disk full?)",
            wrote, new_size, abs_path);
    } else {
        snprintf(summary, sizeof(summary),
            "Edit applied to %s (%zu bytes)\n",
            abs_path, new_size);
    }
    printf("  [tool] %s", summary);

    /* Combine summary + diff for the result text */
    size_t result_text_len = strlen(summary) + (diff_text ? (size_t)diff_len : 0) + 2;
    char *result_text = malloc(result_text_len);
    if (result_text) {
        size_t pos = 0;
        memcpy(result_text + pos, summary, strlen(summary));
        pos += strlen(summary);
        if (diff_text && diff_len > 0) {
            result_text[pos++] = '\n';
            memcpy(result_text + pos, diff_text, diff_len);
            pos += diff_len;
        }
        result_text[pos] = '\0';
        cJSON_AddStringToObject(result, "text", result_text);
        free(result_text);
    } else {
        cJSON_AddStringToObject(result, "text", summary);
    }

    free(new_content); free(buf); free(diff_text); free(abs_path);
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
        "Avoid commands that run forever or require interactive input. "
        "For long-running commands, use timeout prefixes like 'timeout 30s ...' "
        "to manually control the time limit.");
    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *cmd = cJSON_AddObjectToObject(props, "cmd");
    cJSON_AddStringToObject(cmd, "type", "string");
    cJSON_AddStringToObject(cmd, "description", "The shell command to execute");
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
        "Maximum returned size is 100 KB. No approval needed. "
        "Before reading, use shell 'pwd' to confirm the current workspace "
        "directory so you can construct correct relative paths.");
    cJSON *params = cJSON_AddObjectToObject(func, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *path = cJSON_AddObjectToObject(props, "path");
    cJSON_AddStringToObject(path, "type", "string");
    cJSON_AddStringToObject(path, "description", "Path to the file to read");
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
    cJSON_AddStringToObject(content, "description", "String content to write");
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
    cJSON_AddStringToObject(old_p, "description", "Substring to replace");
    cJSON *new_p = cJSON_AddObjectToObject(props, "new");
    cJSON_AddStringToObject(new_p, "type", "string");
    cJSON_AddStringToObject(new_p, "description", "Replacement substring");
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
