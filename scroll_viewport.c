/*
 * scroll_viewport.c
 *
 * Scrolling viewport implementation.
 * Uses ANSI escape sequences for terminal cursor control.
 */

#include "scroll_viewport.h"
#include "utils_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* ── Internal structure ──────────────────────────────────────── */

struct scroll_viewport {
    char  *full_buf;       /* full accumulated output (heap) */
    size_t full_len;
    size_t full_cap;
    int    printed_lines;  /* number of lines currently displayed on terminal */
    int    max_lines;      /* max visible lines before scrolling kicks in */
    int    term_checked;   /* whether terminal height has been measured */
    int    term_width;     /* cached terminal width (columns) */
    int    scroll_mode;    /* 1 after a scroll-mode redraw, 0 in append mode */
    float  ratio;          /* ratio of terminal height (default 0.8) */
    const char *style_pre; /* ANSI prefix for each line (e.g. "\033[90m") */
    const char *style_post;/* ANSI suffix for each line (e.g. "\033[0m") */
};

/* ── Helpers ─────────────────────────────────────────────────── */

/* Compute the number of visual terminal rows a segment of display-width
 * w occupies: ceil(w / term_w), at least 1.
 *
 * IMPORTANT: the viewport always emits "\r\n" after every printed line
 * (never a bare '\n').  The '\r' resets the column and cancels any
 * pending auto-wrap, so the following '\n' advances exactly one row on
 * every terminal - even when the line's width is an exact multiple of
 * term_w.  (A bare '\n' after a full-width row is terminal-dependent:
 * xterm-class terminals wrap first and advance TWO rows, others one.
 * That ambiguity is why we never emit a bare '\n'.)
 *
 * The followed_by_nl parameter is therefore irrelevant to the count and
 * is kept only to keep call sites readable. */
static int seg_visual_lines(int w, int term_w, int followed_by_nl) {
    (void)followed_by_nl;
    if (w <= 0) return 1;
    int lines = (w + term_w - 1) / term_w;
    return lines < 1 ? 1 : lines;
}

/* Print a slice of the buffer to stdout, converting every '\n' into
 * "\r\n" so the on-screen layout matches estimate_visual_lines()
 * exactly (see seg_visual_lines above). */
static void print_slice(const char *s, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            if (i > start)
                fwrite(s + start, 1, i - start, stdout);
            fwrite("\r\n", 1, 2, stdout);
            start = i + 1;
        }
    }
    if (len > start)
        fwrite(s + start, 1, len - start, stdout);
}
/* Find the byte offset within a single logical line s[0..len) (no '\n')
 * where the visual column reaches `col` (i.e. after skipping `col`
 * columns of display width).  Returns the offset of the first character
 * whose STARTING column is >= col, so the tail never begins mid-character.
 * If the line is shorter than col columns, returns len. */
static size_t visual_offset(const char *s, size_t len, int col) {
    size_t off = 0;
    int vcol = 0;
    while (off < len && vcol < col) {
        int bytes;
        int w = utf8_char_width(s + off, &bytes);
        if (bytes <= 0) bytes = 1;
        if (w < 0) w = 1;      /* stray control byte - count as one */
        vcol += w;
        off += (size_t)bytes;
    }
    if (off > len) off = len;
    return off;
}

/* Estimate the number of visual terminal lines the buffer occupies.
 * Splits by '\n', computes the display width of each segment via
 * utf8_string_width(), and sums visual lines for each segment.
 * Returns at least 1 if there's any content. */
static int estimate_visual_lines(const char *s, size_t len, int term_w) {
    if (!s || len == 0 || term_w <= 0) return 0;
    int total = 0;
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && s[eol] != '\n') eol++;
        /* Build a temporary null-terminated string for this segment */
        size_t seg_len = eol - i;
        int followed_by_nl = (eol < len && s[eol] == '\n');
        if (seg_len > 0) {
            /* Temporarily null-terminate (safe because we'll restore) */
            char *p = (char *)s + eol;
            char old = *p;
            *p = '\0';
            int w = utf8_string_width(s + i);
            *p = old;
            total += seg_visual_lines(w, term_w, followed_by_nl);
        } else {
            total++;  /* empty line */
        }
        if (followed_by_nl) {
            i = eol + 1;
        } else {
            break;
        }
    }
    return total;
}

/* Get terminal width from TIOCGWINSZ, or default to 80 */
static int get_term_width(void) {
    struct winsize ws;
    int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return (int)ws.ws_col;
    }
    return 80;
}

/* Get terminal height from TIOCGWINSZ, or default to 24 */
int scroll_viewport_term_height(void) {
    struct winsize ws;
    int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
            return (int)ws.ws_row;
    }
    return 24;
}

/* Ensure buffer has room for 'needed' bytes (incl. NUL) */
static int buf_ensure(char **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return 0;
    size_t new_cap = *cap ? *cap * 2 : 4096;
    while (new_cap < needed) new_cap *= 2;
    char *nb = realloc(*buf, new_cap);
    if (!nb) return -1;
    *buf = nb;
    *cap = new_cap;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

scroll_viewport_t *scroll_viewport_new(void) {
    return scroll_viewport_new_with_ratio(SCROLL_VIEWPORT_DEFAULT_RATIO);
}

scroll_viewport_t *scroll_viewport_new_with_ratio(float ratio) {
    if (ratio <= 0.0f || ratio > 1.0f) ratio = SCROLL_VIEWPORT_DEFAULT_RATIO;
    scroll_viewport_t *vp = calloc(1, sizeof(*vp));
    if (!vp) return NULL;
    vp->ratio = ratio;
    vp->max_lines = 0;   /* lazy-init on first feed */
    return vp;
}

void scroll_viewport_set_style(scroll_viewport_t *vp,
                                 const char *prefix,
                                 const char *suffix) {
    if (!vp) return;
    vp->style_pre  = prefix  ? prefix  : "";
    vp->style_post = suffix ? suffix : "";
}

char *scroll_viewport_finish(scroll_viewport_t *vp, size_t *out_len) {
    if (!vp) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    char *buf = vp->full_buf;
    if (out_len) *out_len = vp->full_len;

    /*
     * Do NOT try to clear the viewport from the terminal with ANSI
     * cursor-move sequences here.  A signal handler (e.g. Ctrl+C) may
     * have printed extra text via raw write() between viewport updates,
     * shifting the cursor position - any cursor-relative clearing would
     * land in the wrong place and cause visual corruption.
     *
     * Instead, just leave the output visible on screen.  Ensure cursor
     * is on a fresh line so subsequent output doesn't run into the last
     * line of viewport content.
     */
    if (vp->full_len > 0 && vp->full_buf[vp->full_len - 1] != '\n') {
        printf("\r\n");
        fflush(stdout);
    }

    free(vp);
    return buf;
}

void scroll_viewport_feed(const char *chunk, int len, void *user_data) {
    scroll_viewport_t *vp = (scroll_viewport_t *)user_data;
    if (!vp || !chunk || len <= 0) return;

    /* ── Lazy-init terminal dimensions on first call ── */
    if (!vp->term_checked) {
        int h = scroll_viewport_term_height();
        vp->max_lines = (int)(h * vp->ratio);
        if (vp->max_lines < 3) vp->max_lines = 3;
        vp->term_width = get_term_width();
        if (vp->term_width < 10) vp->term_width = 80;
        vp->term_checked = 1;
    }

    /* ── Append to full buffer ── */
    size_t needed = vp->full_len + (size_t)len + 1;
    if (buf_ensure(&vp->full_buf, &vp->full_cap, needed) != 0)
        return;   /* OOM - skip this chunk */

    size_t chunk_start = vp->full_len;
    memcpy(vp->full_buf + chunk_start, chunk, (size_t)len);
    vp->full_len += (size_t)len;
    vp->full_buf[vp->full_len] = '\0';

    /* Strip ANSI escapes from the newly appended portion so they
     * never reach the terminal (prevents cursor/screen corruption). */
    strip_ansi_escapes((uint8_t *)(vp->full_buf + chunk_start), (size_t)len);
    vp->full_len = chunk_start + strlen(vp->full_buf + chunk_start);
    size_t stripped_len = vp->full_len - chunk_start;

    /* ── Estimate visual lines (accounts for terminal word-wrap) ── */
    int total_lines = estimate_visual_lines(vp->full_buf, vp->full_len,
                                             vp->term_width);

    const char *pre  = vp->style_pre  ? vp->style_pre  : "";
    const char *post = vp->style_post ? vp->style_post : "";

    if (total_lines <= vp->max_lines) {
        /* ── Normal mode: just print the chunk ──
         * Newlines are emitted as "\r\n" so the on-screen layout matches
         * estimate_visual_lines() on every terminal (see seg_visual_lines). */
        vp->scroll_mode = 0;
        printf("%s", pre);
        if (stripped_len > 0)
            print_slice(vp->full_buf + chunk_start, stripped_len);
        printf("%s", post);
        fflush(stdout);
        vp->printed_lines = total_lines;
        return;
    }

    /* ── Scroll mode: redraw viewport ── */

    /* Move the cursor back to the top of the previously printed area.
     *
     * Append mode: the cursor sits ON the last content row (buffer has no
     * trailing '\n') or one row below it (buffer ends with '\n').  We need
     * to reach the FIRST content row, hence the -1 adjustment.  This is the
     * off-by-one that used to make the redraw drift upward by one row at
     * the append→scroll transition.
     *
     * Scroll mode: the previous redraw always ended with "\r\n", so the
     * cursor is exactly max_lines rows below the redraw top. */
    int move_up;
    if (vp->scroll_mode) {
        move_up = vp->printed_lines;
    } else {
        /* The cursor was positioned by the PREVIOUS feed, so inspect the
         * buffer state before this chunk was appended (chunk_start). */
        int ends_with_nl = (chunk_start > 0 &&
                            vp->full_buf[chunk_start - 1] == '\n');
        move_up = vp->printed_lines - (ends_with_nl ? 0 : 1);
    }
    if (move_up < 0) move_up = 0;
    if (move_up > vp->max_lines * 2)   /* safety net against estimator bugs */
        move_up = vp->max_lines * 2;
    if (move_up > 0)
        printf("\033[%dA", move_up);

    /* Clear from cursor to end of screen */
    printf("\r\033[J");
    vp->scroll_mode = 1;

    /* Calculate visible area: reserve one line for the scroll indicator */
    int visible_lines = vp->max_lines - 1;
    int hidden = total_lines - visible_lines;

    /* Print scroll indicator (always dim, independent of content style) */
    printf("\033[90m... (%d lines above, %d total)\033[0m\r\n", hidden, total_lines);

    /* Walk buffer by logical lines, skipping visual lines to show
     * only the last (visible_lines) visual lines. */
    int target_skip = total_lines - visible_lines;
    if (target_skip < 0) target_skip = 0;

    size_t pos = 0;
    int visual_skipped = 0;
    int lines_emitted = 0;

    while (pos < vp->full_len) {
        size_t eol = pos;
        while (eol < vp->full_len && vp->full_buf[eol] != '\n') eol++;
        size_t seg_len = eol - pos;
        int followed_by_nl = (eol < vp->full_len && vp->full_buf[eol] == '\n');

        /* Calculate visual lines this logical line occupies */
        int seg_visual = 1;
        if (seg_len > 0) {
            char saved = vp->full_buf[eol];
            vp->full_buf[eol] = '\0';
            int w = utf8_string_width(vp->full_buf + pos);
            vp->full_buf[eol] = saved;
            seg_visual = seg_visual_lines(w, vp->term_width, followed_by_nl);
        }

        if (visual_skipped < target_skip) {
            /* Still in the skip zone */
            int can_skip = target_skip - visual_skipped;
            if (seg_visual <= can_skip) {
                visual_skipped += seg_visual;
            } else {
                /* This logical line straddles the boundary - print its tail.
                 * The tail starts at the true visual wrap boundary so the
                 * number of rows it actually occupies equals
                 * seg_visual - skip_here, keeping the redraw exactly
                 * max_lines rows tall. */
                int skip_here = can_skip;
                size_t offset = visual_offset(vp->full_buf + pos, seg_len,
                                              skip_here * vp->term_width);
                printf("%s", pre);
                printf("%.*s", (int)(seg_len - offset), vp->full_buf + pos + offset);
                printf("%s\r\n", post);
                lines_emitted++;
                visual_skipped += seg_visual;
            }
        } else {
            /* Fully visible - print entire logical line */
            if (seg_len > 0) {
                printf("%s", pre);
                printf("%.*s", (int)seg_len, vp->full_buf + pos);
                printf("%s\r\n", post);
            } else {
                printf("%s\r\n%s", pre, post);
            }
            lines_emitted++;
            visual_skipped += seg_visual;
        }

        if (followed_by_nl)
            pos = eol + 1;
        else
            break;
    }

    vp->printed_lines = vp->max_lines;  /* indicator (1) + visible content lines */
    fflush(stdout);
}
