/*
 * scroll_viewport.h
 *
 * Scrolling viewport for real-time terminal output.
 * When output exceeds a threshold (default 80% of terminal height),
 * switches to viewport mode: oldest lines scroll off the top, newest
 * lines appear at bottom, with a scroll indicator line.
 *
 * Usage:
 *
 *   // Create a viewport
 *   scroll_viewport_t *vp = scroll_viewport_new();
 *
 *   // Feed chunks of data as they arrive
 *   scroll_viewport_feed(vp, chunk, len);
 *
 *   // When done, get the full accumulated output and clean up
 *   char *full_output = scroll_viewport_finish(vp, NULL);
 *   // ... use full_output ...
 *   free(full_output);
 */

#ifndef SCROLL_VIEWPORT_H
#define SCROLL_VIEWPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ───────────────────────────────────────────── */
typedef struct scroll_viewport scroll_viewport_t;

/* ── Configuration ───────────────────────────────────────────── */

/* Default ratio of terminal height used as max visible lines */
#define SCROLL_VIEWPORT_DEFAULT_RATIO  0.8f

/* ── Public API ──────────────────────────────────────────────── */

/*
 * Create a new scrolling viewport.
 * Uses the default ratio (80% of terminal height).
 * Returns NULL on allocation failure.
 */
scroll_viewport_t *scroll_viewport_new(void);

/*
 * Create a viewport with a custom ratio (e.g. 0.7 for 70%).
 * ratio must be > 0 and <= 1.0.
 */
scroll_viewport_t *scroll_viewport_new_with_ratio(float ratio);

/*
 * Free all resources and reset the terminal cursor.
 * If out_len is not NULL, the length of the accumulated output
 * is written there.
 * Returns a malloc'd buffer with the full accumulated output.
 * The caller must free() it.
 */
char *scroll_viewport_finish(scroll_viewport_t *vp, size_t *out_len);

/*
 * Feed a chunk of data.  The viewport prints to stdout in real-time
 * using normal append or scrolling mode as needed.
 *
 * Signature matches llm_popen_output_cb_t so it can be passed directly
 * as a callback to llm_runtime_popen (user_data is the scroll_viewport_t*).
 */
void scroll_viewport_feed(const char *chunk, int len, void *user_data);

/*
 * Set ANSI style prefix/suffix for content lines.
 * Every line printed by the viewport will be wrapped as:
 *   <prefix><line content><suffix>
 * Pass NULL for no styling (default).
 * The caller must keep the strings alive for the viewport's lifetime.
 */
void scroll_viewport_set_style(scroll_viewport_t *vp,
                                const char *prefix,
                                const char *suffix);

/*
 * Get the terminal height (number of rows) from TIOCGWINSZ.
 * Returns 24 on failure.
 */
int scroll_viewport_term_height(void);

#ifdef __cplusplus
}
#endif

#endif /* SCROLL_VIEWPORT_H */
