/* vt_regress.c — regression test for scroll_viewport line counting.
 * Runs the SAME feed sequence against two terminal models:
 *   MODE_PLAIN : '\n' always advances exactly 1 row
 *   MODE_XTERM : '\n' after a full-width row advances 2 rows (xterm quirk)
 * and asserts invariants after every feed:
 *   - the prompt row (row 0) is never overwritten
 *   - the cursor never lands inside the viewport area above the indicator
 *   - in scroll mode the indicator sits at row 1 and the cursor at row 1+max_lines
 */
#include "scroll_viewport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROWS 24
#define COLS 80
#define MAXLINES 7   /* 24 * 0.3 = 7 */

typedef struct {
    char grid[ROWS][COLS + 1];
    int  r, c, pending;
    int  xterm;   /* xterm-style LF-after-full-row model */
} vt_t;
static vt_t g_vt;
static int g_fail = 0;

static void vt_init(int xterm) {
    for (int i = 0; i < ROWS; i++) { memset(g_vt.grid[i], ' ', COLS); g_vt.grid[i][COLS] = 0; }
    g_vt.r = 0; g_vt.c = 0; g_vt.pending = 0; g_vt.xterm = xterm;
    memcpy(g_vt.grid[0], "User > prompt line", 18);
    g_vt.r = 1; g_vt.c = 0;
}
static void vt_clear_from(void) {
    for (int j = g_vt.c; j < COLS; j++) g_vt.grid[g_vt.r][j] = ' ';
    for (int i = g_vt.r + 1; i < ROWS; i++) memset(g_vt.grid[i], ' ', COLS);
}
static void vt_put(char ch) {
    if (ch == '\n') {
        if (g_vt.xterm && g_vt.pending) { g_vt.r++; g_vt.c = 0; g_vt.pending = 0; }
        g_vt.r++; g_vt.c = 0; g_vt.pending = 0;
        if (g_vt.r >= ROWS) g_vt.r = ROWS - 1;
        return;
    }
    if (ch == '\r') { g_vt.c = 0; g_vt.pending = 0; return; }
    if (ch < 0x20 || ch == 0x7f) return;
    if (g_vt.pending) { g_vt.r++; g_vt.c = 0; g_vt.pending = 0; if (g_vt.r >= ROWS) g_vt.r = ROWS - 1; }
    g_vt.grid[g_vt.r][g_vt.c++] = ch;
    if (g_vt.c >= COLS) g_vt.pending = 1;
}
static void vt_bytes(const char *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == 0x1b) {
            if (i + 1 < n && buf[i + 1] == '[') {
                i += 2; int n1 = 0;
                while (i < n && buf[i] >= '0' && buf[i] <= '9') { n1 = n1 * 10 + (buf[i] - '0'); i++; }
                if (i < n) {
                    char f = buf[i]; i++;
                    if (f == 'A') { g_vt.r -= n1 > 0 ? n1 : 1; if (g_vt.r < 0) g_vt.r = 0; }
                    else if (f == 'J') { if (n1 == 0 || n1 == 2) vt_clear_from(); }
                    else if (f == 'K') { for (int j = g_vt.c; j < COLS; j++) g_vt.grid[g_vt.r][j] = ' '; }
                    else if (f == 'm') {}
                    else if (f == 'G') { if (n1 > 0) { g_vt.c = n1 - 1; if (g_vt.c >= COLS) g_vt.c = COLS - 1; } }
                }
            } else i++;
        } else { vt_put((char)ch); i++; }
    }
}
static void feed_capture(scroll_viewport_t *vp, const char *chunk) {
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); exit(1); }
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(p[1], STDOUT_FILENO); close(p[1]);
    scroll_viewport_feed(chunk, (int)strlen(chunk), vp);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO); close(saved);
    char buf[131072];
    ssize_t n = read(p[0], buf, sizeof(buf)); close(p[0]);
    if (n > 0) vt_bytes(buf, (size_t)n);
}

static void check(int feed, const char *model, int scroll_expected) {
    /* prompt row must be intact */
    if (memcmp(g_vt.grid[0], "User > prompt line", 18) != 0) {
        printf("FAIL[%s] feed %d: prompt row overwritten (r=%d c=%d)\n", model, feed, g_vt.r, g_vt.c);
        g_fail = 1;
    }
    /* cursor must never be above row 1 or below row 1+MAXLINES */
    if (g_vt.r < 1 || g_vt.r > 1 + MAXLINES) {
        printf("FAIL[%s] feed %d: cursor out of bounds r=%d c=%d (expect 1..%d)\n",
               model, feed, g_vt.r, g_vt.c, 1 + MAXLINES);
        g_fail = 1;
    }
    if (scroll_expected) {
        /* indicator must be at row 1, cursor at row 1+MAXLINES */
        if (memcmp(g_vt.grid[1], "...", 3) != 0) {
            printf("FAIL[%s] feed %d: scroll indicator not at row 1: '%.20s'\n", model, feed, g_vt.grid[1]);
            g_fail = 1;
        }
        if (g_vt.r != 1 + MAXLINES) {
            printf("FAIL[%s] feed %d: cursor r=%d expected %d\n", model, feed, g_vt.r, 1 + MAXLINES);
            g_fail = 1;
        }
    } else {
        /* normal mode: indicator must NOT be visible */
        for (int i = 1; i <= ROWS - 1; i++)
            if (memcmp(g_vt.grid[i], "...", 3) == 0) {
                printf("FAIL[%s] feed %d: stray indicator at row %d in normal mode\n", model, feed, i);
                g_fail = 1;
            }
    }
}

int main(void) {
    const char *chunks[] = {
        "Let me think about this carefully. The user asks about the scroll viewport ",
        "logic and whether it re-renders lines correctly when the output exceeds the ",
        "visible area of the terminal.\n",
        "First, I need to consider how the terminal wraps lines that are exactly as ",
        "wide as the terminal itself. That is a classic off-by-one source when count",
        "ing visual rows.\n",
        NULL };
    static char a80[81], b80[81];
    memset(a80, 'A', 80); a80[80] = 0;
    memset(b80, 'B', 80); b80[80] = 0;

    /* build a long stream: 40 lines with mixed widths, incl. exact-80 */
    char stream[8192]; size_t sn = 0;
    for (int i = 0; i < 40; i++) {
        int w = (i % 5 == 0) ? 80 : (30 + (i * 37) % 100);
        for (int j = 0; j < w; j++) stream[sn++] = 'a' + (i % 26);
        stream[sn++] = '\n';
    }
    stream[sn] = 0;

    for (int model = 0; model < 2; model++) {
        const char *mname = model ? "XTERM" : "PLAIN";
        vt_init(model);
        scroll_viewport_t *vp = scroll_viewport_new_with_ratio(0.3f);
        scroll_viewport_set_style(vp, "\033[90m", "\033[0m");

        int feed = 0;
        for (int i = 0; chunks[i]; i++) {
            feed_capture(vp, chunks[i]);
            feed++;
            check(feed, mname, 0);
        }
        /* feed the 80-char lines + newline, then the long stream in small chunks */
        feed_capture(vp, a80); feed++; check(feed, mname, 0);
        feed_capture(vp, "\n"); feed++; check(feed, mname, 0);
        feed_capture(vp, b80); feed++; check(feed, mname, 1);  /* transition */
        feed_capture(vp, "x"); feed++; check(feed, mname, 1);  /* transition into scroll */
        feed_capture(vp, "y"); feed++; check(feed, mname, 1);
        feed_capture(vp, "z"); feed++; check(feed, mname, 1);

        /* long stream, chopped into 7-byte chunks (many partial lines) */
        size_t off = 0;
        while (off < sn) {
            size_t n = 7;
            if (off + n > sn) n = sn - off;
            char tmp[8]; memcpy(tmp, stream + off, n); tmp[n] = 0;
            feed_capture(vp, tmp);
            feed++;
            check(feed, mname, 1);
            off += n;
        }
        size_t len = 0; char *b = scroll_viewport_finish(vp, &len); free(b);
        printf("model %s: %d feeds, %s\n", mname, feed, g_fail ? "FAILURES" : "all invariants OK");
    }
    return g_fail ? 1 : 0;
}
