/*
 * stream_md_renderer.h
 *
 * Incremental streaming Markdown → ANSI renderer.
 * Feeds text character-by-character, re-parses on every chunk,
 * and produces ANSI-styled output for terminal display.
 *
 * Provides a high-level md_display_t that auto-manages diff-based
 * in-place terminal updates — ideal for AI chat streaming UIs.
 */

#ifndef STREAM_MD_RENDERER_H
#define STREAM_MD_RENDERER_H

#include "sds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Incremental Markdown Renderer ─────────────────────────── */
/* Accumulates text chunks and re-renders to ANSI on every feed. */

typedef struct md_renderer md_renderer_t;

/* Create a renderer with the given max width (0 = unlimited).
 * The renderer accumulates input internally. */
md_renderer_t *md_renderer_new(int max_width);

/* Free all resources. */
void md_renderer_free(md_renderer_t *r);

/* Reset accumulated text to empty. Keeps max_width. */
void md_renderer_reset(md_renderer_t *r);

/* Feed incremental markdown text. Returns the fully rendered ANSI
 * output as an sds string. Caller must sdsfree() the result. */
sds  md_renderer_feed(md_renderer_t *r, const char *text, int len);

/* Get the current rendered output (owned by the renderer, do NOT free). */
sds  md_renderer_output(md_renderer_t *r);

/* Get the raw accumulated markdown text (for debugging). */
sds  md_renderer_raw(md_renderer_t *r);

/* ── Diff helpers (exposed for advanced use) ───────────────── */

/* Count \n characters in an sds string (== number of lines minus one). */
int  md_count_lines(const char *s);

/* Compare two rendered outputs and return the index of the first
 * differing line. Returns -1 if identical. */
int  md_compute_diff(const char *old_text, int old_len,
                     const char *new_text, int new_len);

/* Print a diff-based in-place update to stdout.
 * Uses CSI cursor movement to only redraw changed lines.
 * old/new are sds strings; old_lines / new_lines are their \n counts. */
void md_print_diff(const char *old_text, int old_lines,
                   const char *new_text, int new_lines,
                   int first_changed);

/* ── High-level display helper ─────────────────────────────── */
/* md_display_t wraps a renderer + previous-output state so the
 * caller can simply call md_display_feed() on each chunk;
 * diff-based terminal updates happen automatically. */

typedef struct {
    md_renderer_t *r;
    char          *prev;        /* malloc'd copy of previous rendered output */
    int            prev_len;    /* byte length of prev */
    int            prev_lines;  /* line count of prev */
} md_display_t;

/* Initialize a display. max_width is passed to the internal renderer. */
void md_display_init(md_display_t *d, int max_width);

/* Free the display and its internal renderer. */
void md_display_free(md_display_t *d);

/* Feed one chunk of markdown text. Prints the diff update to stdout.
 * On the first call, prints the full rendered output.
 * On subsequent calls, updates only changed lines in-place. */
void md_display_feed(md_display_t *d, const char *text, int len);

/* Reset display state: clears accumulated text and prev output.
 * Useful when a new response begins. */
void md_display_reset(md_display_t *d);

/* ── Utilities ────────────────────────────────────────────── */

/* Get terminal width from TIOCGWINSZ or default to 80. */
int  md_get_terminal_width(void);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_MD_RENDERER_H */
