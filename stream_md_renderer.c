#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "md4c.h"
#include "utils_utf8.h"
#include "sds.h"
#include "stream_md_renderer.h"

#define ANSI_RESET      "\033[0m"
#define ANSI_BOLD       "\033[1m"
#define ANSI_DIM        "\033[2m"
#define ANSI_ITALIC     "\033[3m"
#define ANSI_UNDERLINE  "\033[4m"
#define ANSI_STRIKE     "\033[9m"
#define ANSI_INVERT     "\033[7m"

#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"

#define ANSI_BRIGHT_BLACK   "\033[90m"
#define ANSI_BRIGHT_RED     "\033[91m"
#define ANSI_BRIGHT_GREEN   "\033[92m"
#define ANSI_BRIGHT_YELLOW  "\033[93m"
#define ANSI_BRIGHT_BLUE    "\033[94m"
#define ANSI_BRIGHT_MAGENTA "\033[95m"
#define ANSI_BRIGHT_CYAN    "\033[96m"
#define ANSI_BRIGHT_WHITE   "\033[97m"

#define MAX_STYLES 64
#define MAX_LISTS 16
#define MAX_TCELLS 512
#define MAX_TROWS 128
#define MIN_COL_WIDTH 3



/* ── list tracking ───────────────────────────────────────────── */

typedef struct {
    int ordered;
    char bullet;
    unsigned start;
    unsigned count;
} list_info;

/* ── table capture ──────────────────────────────────────────── */

typedef struct {
    sds    styled;    /* full styled cell content (sds string) */
    unsigned col;
    int    is_header;
    char **wrapped;   /* wrapped visual lines (arena-allocated) */
    int    nwrapped;  /* number of wrapped lines */
} TCell;

/* ── render context ──────────────────────────────────────────── */

typedef struct {
    Arena *arena;       /* arena for all sds allocations */
    sds out;            /* output buffer (sds string) */
    int bol;

    int heading_level;

    int in_blockquote;

    list_info list_stack[MAX_LISTS];
    int list_depth;
    int in_li_count;
    int li_marker_emitted;
    int li_task_mark;
    int li_indent_stack[MAX_LISTS];  /* continuation indent for each LI nesting level */

    int in_code_block;
    int code_is_fenced;
    char code_lang[64];

    int in_html_block;

    int max_width;     /* maximum rendering width (0 = unlimited) */

    /* table state */
    int tbl_active;
    int tbl_thead;
    unsigned tbl_cols;
    MD_ALIGN tbl_aligns[32];
    int tbl_in_cell;
    unsigned tbl_col;
    sds tbl_cell_out;  /* non-NULL: capture cell output here (parallel to c->out) */

    TCell tbl_cells[MAX_TCELLS];
    int tbl_ncell;
    int tbl_row_start[MAX_TROWS];
    int tbl_nrow;

    MD_SPANTYPE styles[MAX_STYLES];
    int style_depth;
} mdrenderer_ctx;

/* ── style helpers ───────────────────────────────────────────── */

static const char *heading_ansi(int level) {
    switch (level) {
        case 1:  return ANSI_BOLD ANSI_UNDERLINE ANSI_BRIGHT_YELLOW;
        case 2:  return ANSI_BOLD ANSI_BRIGHT_CYAN;
        case 3:  return ANSI_BOLD ANSI_BRIGHT_GREEN;
        case 4:  return ANSI_BOLD ANSI_BRIGHT_BLUE;
        case 5:  return ANSI_BOLD ANSI_BRIGHT_MAGENTA;
        case 6:  return ANSI_BOLD ANSI_BRIGHT_BLACK;
        default: return ANSI_RESET;
    }
}

static const char *span_ansi(MD_SPANTYPE type) {
    switch (type) {
        case MD_SPAN_EM:          return ANSI_ITALIC ANSI_BRIGHT_CYAN;
        case MD_SPAN_STRONG:      return ANSI_BOLD;
        case MD_SPAN_CODE:        return ANSI_BRIGHT_GREEN;
        case MD_SPAN_DEL:         return ANSI_STRIKE;
        case MD_SPAN_A:           return ANSI_UNDERLINE ANSI_BRIGHT_BLUE;
        case MD_SPAN_IMG:         return ANSI_DIM ANSI_YELLOW;
        case MD_SPAN_U:           return ANSI_UNDERLINE;
        case MD_SPAN_SUPERSCRIPT: return ANSI_DIM;
        case MD_SPAN_SUBSCRIPT:   return ANSI_DIM;
        case MD_SPAN_SPOILER:     return ANSI_INVERT;
        case MD_SPAN_LATEXMATH:   return ANSI_MAGENTA;
        case MD_SPAN_LATEXMATH_DISPLAY: return ANSI_BOLD ANSI_MAGENTA;
        case MD_SPAN_WIKILINK:    return ANSI_UNDERLINE ANSI_BRIGHT_GREEN;
        default:                  return "";
    }
}

/* ── low-level output (all append into ctx->out) ──────────────── */

static void out_raw(mdrenderer_ctx *c, const char *s, size_t len) {
    if (c->tbl_cell_out) {
        c->tbl_cell_out = sdscatlen(c->tbl_cell_out, s, len);
    } else {
        c->out = sdscatlen(c->out, s, len);
    }
}
static void out_str(mdrenderer_ctx *c, const char *s) {
    size_t _l = strlen(s);
    if (c->tbl_cell_out) { c->tbl_cell_out = sdscatlen(c->tbl_cell_out, s, _l); }
    else { c->out = sdscatlen(c->out, s, _l); }
}
static void out_char(mdrenderer_ctx *c, char ch) {
    if (c->tbl_cell_out) { c->tbl_cell_out = sdscatlen(c->tbl_cell_out, &ch, 1); }
    else { c->out = sdscatlen(c->out, &ch, 1); }
}

static void out_repeat(mdrenderer_ctx *c, char ch, int n) {
    for (int i = 0; i < n; i++) out_char(c, ch);
}

static void out_repeat_str(mdrenderer_ctx *c, const char *s, int n) {
    for (int i = 0; i < n; i++) out_str(c, s);
}

/* ── reapply full style context ──────────────────────────────── */

static void restyle(mdrenderer_ctx *c) {
    out_str(c, ANSI_RESET);
    if (c->heading_level > 0) out_str(c, heading_ansi(c->heading_level));
    if (c->in_blockquote)     out_str(c, ANSI_CYAN);
    if (c->in_code_block)     out_str(c, ANSI_GREEN);
    if (c->in_html_block)     out_str(c, ANSI_DIM);
    for (int i = 0; i < c->style_depth; i++) out_str(c, span_ansi(c->styles[i]));
}

/* count visible width in a possibly ANSI-escaped string (UTF-8 aware) */
static int dispw(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '\033') {
            while (*s && *s != 'm') s++;
            if (*s) s++;
        } else {
            int bytes;
            int w = utf8_char_width(s, &bytes);
            if (w > 0) n += w;
            s += bytes;
        }
    }
    return n;
}

/* ── wrap ANSI-styled text into lines ────────────────────────── */

/* Extract leading ANSI escape sequences from styled text.
 * Non-reset sequences are accumulated; reset (\033[0m) clears all.
 * Returns the number of bytes consumed (skip past the leading ANSI). */
static size_t extract_leading_ansi(const char *styled, size_t len,
                                    char *prefix, int *plen) {
    size_t si = 0;
    *plen = 0;
    prefix[0] = '\0';
    while (si < len && styled[si] == '\033') {
        size_t ss = si;
        while (si < len && styled[si] != 'm') si++;
        if (si < len) si++;  /* include the 'm' */
        int sl = (int)(si - ss);
        /* Reset → clear accumulated prefix */
        if (sl >= 3 && styled[ss + 2] == '0' &&
            (sl == 4 || (sl > 4 && styled[ss + 3] == 'm'))) {
            *plen = 0;
            prefix[0] = '\0';
        } else if (*plen + sl < 511) {
            memcpy(prefix + *plen, styled + ss, sl);
            *plen += sl;
            prefix[*plen] = '\0';
        }
    }
    return si;
}

static char **wrap_styled_text(Arena *a, const char *styled, int max_w, int *nlines) {
    if (!styled) styled = "";
    if (max_w < MIN_COL_WIDTH) max_w = MIN_COL_WIDTH;

    size_t len = strlen(styled);

    /* Extract leading ANSI codes as the initial style prefix.
     * This fixes code blocks where \033[92m appears at position 0
     * and the old scan missed it (si < line_start, line_start=0). */
    char init_prefix[512];
    int init_plen;
    size_t ansi_end = extract_leading_ansi(styled, len, init_prefix, &init_plen);
    size_t pos = ansi_end;

    char **lines = NULL;
    int line_cap = 0;
    *nlines = 0;

    while (pos < len) {
        size_t line_start = pos;
        int col = 0;
        int last_break = -1;

        int line_full = 0;
        while (pos < len && !line_full) {
            unsigned char c = (unsigned char)styled[pos];

            if (c == '\033') {
                while (pos < len && styled[pos] != 'm') pos++;
                if (pos < len) pos++;
                continue;
            }

            if (c == ' ') last_break = (int)pos;

            int cb, cw;
            cw = utf8_char_width(styled + pos, &cb);
            if (cw <= 0) { cw = 1; cb = 1; }

            if (col + cw > max_w) {
                if (last_break >= 0 && last_break > (int)line_start)
                    pos = (size_t)(last_break + 1);
                line_full = 1;
            } else {
                col += cw;
                pos += cb;
            }
        }

        size_t line_end = pos;
        while (line_end > line_start && styled[line_end - 1] == ' ') line_end--;

        /* Build prefix: init_prefix + any ANSI codes between the leading
         * ANSI and the current line_start that are not resets. */
        char prefix[512];
        int plen = init_plen;
        if (plen > 0 && plen < (int)sizeof(prefix))
            memcpy(prefix, init_prefix, (size_t)plen);
        for (size_t si = ansi_end; si < line_start;) {
            if (styled[si] == '\033') {
                size_t ss = si;
                while (si < line_start && styled[si] != 'm') si++;
                if (si < line_start) si++;
                int sl = (int)(si - ss);
                if (sl >= 3 && styled[ss + 2] == '0' &&
                    (sl == 4 || styled[ss + 3] == 'm')) {
                    plen = 0;
                } else if (plen + sl < (int)sizeof(prefix) - 1) {
                    memcpy(prefix + plen, styled + ss, sl);
                    plen += sl;
                    prefix[plen] = '\0';
                }
            } else {
                si++;
            }
        }

        /* build line: prefix + content + ANSI_RESET */
        size_t content_len = line_end - line_start;
        char *line = (char *)arena_alloc(a, plen + content_len + 5);
        if (plen > 0) memcpy(line, prefix, plen);
        if (content_len > 0)
            memcpy(line + plen, styled + line_start, content_len);
        memcpy(line + plen + content_len, "\033[0m", 4);
        line[plen + content_len + 4] = '\0';

        if (*nlines >= line_cap) {
            size_t old_cap_bytes = (size_t)line_cap * sizeof(char *);
            line_cap = line_cap ? line_cap * 2 : 8;
            lines = (char **)arena_realloc(a, lines, old_cap_bytes,
                                            (size_t)line_cap * sizeof(char *));
        }
        lines[*nlines] = line;
        (*nlines)++;

        while (pos < len && styled[pos] == ' ') pos++;
    }

    if (*nlines == 0) {
        lines = (char **)arena_alloc(a, sizeof(char *));
        lines[0] = arena_strdup(a, "");
        *nlines = 1;
    }
    return lines;
}

/* ── handle BOL prefix (blockquote, list marker) ────────────── */

static void handle_bol_prefix(mdrenderer_ctx *c) {
    if (!c->bol) return;

    if (c->in_blockquote) {
        out_str(c, ANSI_CYAN "> " ANSI_RESET);
        restyle(c);
    }

    if (c->in_li_count > 0) {
        if (!c->li_marker_emitted) {
            c->li_marker_emitted = 1;
            if (c->list_depth > 0) {
                list_info *li = &c->list_stack[c->list_depth - 1];

                int marker_indent = (c->list_depth - 1) * 2;
                out_repeat(c, ' ', marker_indent);

                out_str(c, ANSI_YELLOW);
                int marker_width = 0;
                if (li->ordered) {
                    char buf[32];
                    int n = snprintf(buf, sizeof(buf), "%u%c ",
                                     li->start + li->count - 1,
                                     li->bullet ? li->bullet : '.');
                    out_raw(c, buf, n);
                    marker_width = n;
                } else {
                    if (li->bullet == '-' || li->bullet == '+') {
                        out_char(c, li->bullet);
                        out_char(c, ' ');
                        marker_width = 2;
                    } else {
                        out_str(c, "\xe2\x80\xa2 ");
                        marker_width = 2;
                    }
                }
                if (c->li_task_mark) {
                    out_str(c, ANSI_RESET " [");
                    if (c->li_task_mark == 'x' || c->li_task_mark == 'X')
                        out_str(c, ANSI_GREEN "x");
                    else
                        out_char(c, ' ');
                    out_str(c, "]" ANSI_RESET);
                    marker_width += 3;
                    restyle(c);
                }
                c->li_indent_stack[c->in_li_count - 1] = marker_indent + marker_width;
            }
        } else {
            out_repeat(c, ' ', c->li_indent_stack[c->in_li_count - 1]);
        }
    }

    c->bol = 0;
}

/* ── output text with line-prefix support ────────────────────── */

static void emit_text(mdrenderer_ctx *c, const char *text, MD_SIZE size) {
    if (size == 0) return;
    handle_bol_prefix(c);
    out_raw(c, text, size);
}

/* ── forward declaration for terminal height query ─────────── */
static int md_get_terminal_height(void);

/* ════════════════════════════════════════════════════════════════
 * Table rendering helpers
 * ════════════════════════════════════════════════════════════════ */

static void tbl_calc_colw(mdrenderer_ctx *c, int ncols, int *colw) {
    int orig_colw[32] = {0};

    for (int i = 0; i < c->tbl_ncell; i++) {
        TCell *cell = &c->tbl_cells[i];
        int w = sdslen(cell->styled) ? dispw(cell->styled) : 0;
        if (w > orig_colw[cell->col]) orig_colw[cell->col] = w;
    }
    for (int i = 0; i < ncols; i++) {
        if (orig_colw[i] < MIN_COL_WIDTH) orig_colw[i] = MIN_COL_WIDTH;
    }

    memcpy(colw, orig_colw, ncols * sizeof(int));

    if (c->max_width <= 0) return;

    int total_orig = 0;
    for (int i = 0; i < ncols; i++) total_orig += orig_colw[i];
    int borders = ncols + 1;
    int total = total_orig + borders;
    if (total <= c->max_width) return;

    int avail = c->max_width - borders;

    /* Proportional shrink: each column gets share of avail scaled by orig_colw.
     * This avoids the greedy-largest-gap bug where short columns get starved. */
    for (int i = 0; i < ncols; i++) {
        colw[i] = orig_colw[i] * avail / total_orig;
        if (colw[i] < MIN_COL_WIDTH) colw[i] = MIN_COL_WIDTH;
    }

    /* Fix rounding / min-enforcement discrepancies */
    int sum = 0;
    for (int i = 0; i < ncols; i++) sum += colw[i];
    int diff = avail - sum;

    while (diff > 0) {
        /* Give extra pixel to column with largest proportional shortfall */
        int best = -1;
        double best_need = -1.0;
        for (int i = 0; i < ncols; i++) {
            if (colw[i] < orig_colw[i]) {
                double need = (double)(orig_colw[i] - colw[i]) / orig_colw[i];
                if (need > best_need) { best_need = need; best = i; }
            }
        }
        if (best < 0) break;
        colw[best]++;
        diff--;
    }

    while (diff < 0) {
        /* Over-allocation (too many MIN_COL_WIDTH floors): shrink largest */
        int best = -1, biggest = 0;
        for (int i = 0; i < ncols; i++) {
            if (colw[i] > MIN_COL_WIDTH && colw[i] > biggest) {
                biggest = colw[i];
                best = i;
            }
        }
        if (best < 0) break;
        colw[best]--;
        diff++;
    }
}

static void tbl_wrap_cells(mdrenderer_ctx *c, int ncols, const int *colw) {
    (void)ncols;
    for (int i = 0; i < c->tbl_ncell; i++) {
        TCell *cell = &c->tbl_cells[i];
        cell->wrapped = wrap_styled_text(c->arena, cell->styled,
                                         colw[cell->col],
                                         &cell->nwrapped);
    }
}

static void tbl_row_lines(mdrenderer_ctx *c, int *row_lines) {
    for (int ri = 0; ri < c->tbl_nrow; ri++) {
        int start = c->tbl_row_start[ri];
        int end = (ri + 1 < c->tbl_nrow)
                      ? c->tbl_row_start[ri + 1]
                      : c->tbl_ncell;
        int mx = 1;
        for (int cj = start; cj < end; cj++) {
            if (c->tbl_cells[cj].nwrapped > mx)
                mx = c->tbl_cells[cj].nwrapped;
        }
        row_lines[ri] = mx;
    }
}

static int tbl_count_headers(mdrenderer_ctx *c) {
    int n = 0;
    for (int i = 0; i < c->tbl_nrow; i++) {
        int start = c->tbl_row_start[i];
        if (start < c->tbl_ncell && c->tbl_cells[start].is_header)
            n++;
        else
            break;
    }
    return n;
}

static void tbl_border(mdrenderer_ctx *c, const char *left, const char *mid,
                       const char *right, const char *dash,
                       int ncols, const int *colw) {
    out_str(c, left);
    for (int ci = 0; ci < ncols; ci++) {
        out_repeat_str(c, dash, colw[ci]);
        if (ci < ncols - 1) out_str(c, mid);
    }
    out_str(c, right);
    out_str(c, ANSI_RESET "\n");
}

static void tbl_render(mdrenderer_ctx *c, int ncols, const int *colw,
                       const int *row_lines, int header_rows) {
    tbl_border(c, "\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90",
               "\xe2\x94\x80", ncols, colw);

    for (int ri = 0; ri < c->tbl_nrow; ri++) {
        int start = c->tbl_row_start[ri];
        int end = (ri + 1 < c->tbl_nrow)
                      ? c->tbl_row_start[ri + 1]
                      : c->tbl_ncell;
        int rlines = row_lines[ri];

        for (int vl = 0; vl < rlines; vl++) {
            for (int ci = 0; ci < ncols; ci++) {
                out_str(c, ANSI_RESET "\xe2\x94\x82");

                TCell *cell = NULL;
                for (int cj = start; cj < end; cj++) {
                    if (c->tbl_cells[cj].col == (unsigned)ci) {
                        cell = &c->tbl_cells[cj];
                        break;
                    }
                }

                const char *content = "";
                if (cell && vl < cell->nwrapped)
                    content = cell->wrapped[vl];

                int cw = dispw(content);
                int pad = colw[ci] - cw;
                MD_ALIGN a = c->tbl_aligns[ci];

                if (a == MD_ALIGN_RIGHT) {
                    out_repeat(c, ' ', pad);
                    out_str(c, content);
                } else if (a == MD_ALIGN_CENTER) {
                    int left = pad / 2;
                    out_repeat(c, ' ', left);
                    out_str(c, content);
                    out_repeat(c, ' ', pad - left);
                } else {
                    out_str(c, content);
                    out_repeat(c, ' ', pad);
                }
                out_str(c, ANSI_RESET);
            }
            out_str(c, ANSI_RESET "\xe2\x94\x82" ANSI_RESET "\n");
        }

        int need_sep = (ri < header_rows - 1) ||
                       (ri == header_rows - 1 && ri + 1 < c->tbl_nrow) ||
                       (ri >= header_rows && ri < c->tbl_nrow - 1);
        if (need_sep) {
            tbl_border(c, "\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4",
                       "\xe2\x94\x80", ncols, colw);
        }
    }

    tbl_border(c, "\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98",
               "\xe2\x94\x80", ncols, colw);
}

static void tbl_free_cells(mdrenderer_ctx *c) {
    for (int i = 0; i < c->tbl_ncell; i++) {
        c->tbl_cells[i].styled = NULL;
        c->tbl_cells[i].wrapped = NULL;
        c->tbl_cells[i].nwrapped = 0;
    }
    c->tbl_ncell = 0;
    c->tbl_nrow = 0;
}

/* ════════════════════════════════════════════════════════════════
 * MD4C callbacks
 * ════════════════════════════════════════════════════════════════ */

static int enter_block_cb(MD_BLOCKTYPE type, void *detail, void *userdata) {
    mdrenderer_ctx *c = (mdrenderer_ctx *)userdata;

    switch (type) {
    case MD_BLOCK_DOC:
        c->bol = 1;
        break;

    case MD_BLOCK_QUOTE:
        c->in_blockquote = 1;
        c->bol = 1;
        break;

    case MD_BLOCK_UL: {
        MD_BLOCK_UL_DETAIL *d = (MD_BLOCK_UL_DETAIL *)detail;
        if (c->list_depth < MAX_LISTS) {
            c->list_stack[c->list_depth].ordered = 0;
            c->list_stack[c->list_depth].bullet = d->mark;
            c->list_depth++;
        }
        break;
    }
    case MD_BLOCK_OL: {
        MD_BLOCK_OL_DETAIL *d = (MD_BLOCK_OL_DETAIL *)detail;
        if (c->list_depth < MAX_LISTS) {
            c->list_stack[c->list_depth].ordered = 1;
            c->list_stack[c->list_depth].start = d->start;
            c->list_stack[c->list_depth].count = 0;
            c->list_stack[c->list_depth].bullet = d->mark_delimiter;
            c->list_depth++;
        }
        break;
    }
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *ld = (MD_BLOCK_LI_DETAIL *)detail;
        if (c->in_li_count > 0 && !c->bol) out_char(c, '\n');
        c->in_li_count++;
        c->li_marker_emitted = 0;
        c->li_task_mark = ld->is_task ? ld->task_mark : 0;
        if (c->list_depth > 0)
            c->list_stack[c->list_depth - 1].count++;
        c->bol = 1;
        break;
    }

    case MD_BLOCK_H: {
        MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
        c->heading_level = d->level;
        out_str(c, heading_ansi(c->heading_level));
        out_repeat(c, '#', c->heading_level);
        out_char(c, ' ');
        c->bol = 0;
        break;
    }

    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL *)detail;
        c->in_code_block = 1;
        c->code_is_fenced = (d->fence_char != 0);
        out_str(c, c->code_is_fenced ? "```" : "code");
        if (d->lang.size > 0) {
            out_str(c, ANSI_RESET " " ANSI_BRIGHT_WHITE);
            out_raw(c, d->lang.text, d->lang.size);
            size_t n = d->lang.size;
            if (n >= sizeof(c->code_lang)) n = sizeof(c->code_lang) - 1;
            memcpy(c->code_lang, d->lang.text, n);
            c->code_lang[n] = '\0';
        }
        out_str(c, ANSI_RESET "\n");
        out_str(c, ANSI_GREEN);
        break;
    }

    case MD_BLOCK_HTML:
        c->in_html_block = 1;
        break;

    case MD_BLOCK_P:
        c->bol = 1;
        break;

    case MD_BLOCK_HR:
        out_str(c, ANSI_DIM);
        out_repeat_str(c, "\xe2\x94\x80", 72);
        out_str(c, ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_BLOCK_TABLE: {
        MD_BLOCK_TABLE_DETAIL *d = (MD_BLOCK_TABLE_DETAIL *)detail;
        c->tbl_active = 1;
        c->tbl_cols = d->col_count;
        c->tbl_thead = 0;
        c->tbl_ncell = 0;
        c->tbl_nrow = 0;
        c->tbl_col = 0;
        c->tbl_in_cell = 0;
        memset(c->tbl_aligns, 0, sizeof(c->tbl_aligns));
        c->tbl_cell_out = NULL;
        break;
    }
    case MD_BLOCK_THEAD:
        c->tbl_thead = 1;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TBODY:
        c->tbl_thead = 0;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TR:
        c->tbl_row_start[c->tbl_nrow] = c->tbl_ncell;
        c->tbl_col = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (type == MD_BLOCK_TH) {
            MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
            c->tbl_aligns[c->tbl_col] = td->align;
        }
        c->tbl_in_cell = 1;

        /* capture cell output into a parallel buffer; c->out stays as main output */
        c->tbl_cell_out = sdsempty(c->arena);

        c->tbl_cells[c->tbl_ncell].col = c->tbl_col;
        c->tbl_cells[c->tbl_ncell].is_header = (type == MD_BLOCK_TH);
        c->tbl_cells[c->tbl_ncell].wrapped = NULL;
        c->tbl_cells[c->tbl_ncell].nwrapped = 0;
        break;
    }

    default:
        break;
    }
    return 0;
}

static int leave_block_cb(MD_BLOCKTYPE type, void *detail, void *userdata) {
    (void)detail;
    mdrenderer_ctx *c = (mdrenderer_ctx *)userdata;

    switch (type) {
    case MD_BLOCK_QUOTE:
        c->in_blockquote = 0;
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (c->list_depth > 0) c->list_depth--;
        break;

    case MD_BLOCK_LI:
        if (c->in_li_count > 0) c->in_li_count--;
        out_str(c, ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_BLOCK_H:
        out_str(c, ANSI_RESET "\n");
        c->heading_level = 0;
        c->bol = 1;
        break;

    case MD_BLOCK_CODE:
        out_str(c, ANSI_RESET);
        if (c->code_is_fenced)
            out_str(c, "```");
        out_char(c, '\n');
        c->in_code_block = 0;
        c->code_is_fenced = 0;
        c->bol = 1;
        break;

    case MD_BLOCK_HTML:
        c->in_html_block = 0;
        out_char(c, '\n');
        break;

    case MD_BLOCK_P:
    case MD_BLOCK_HR:
        out_char(c, '\n');
        c->bol = 1;
        break;

    case MD_BLOCK_THEAD:
        c->tbl_thead = 0;
        break;

    case MD_BLOCK_TR:
        c->tbl_nrow++;
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (c->tbl_in_cell) {
            c->tbl_cells[c->tbl_ncell].styled = c->tbl_cell_out;
            c->tbl_cell_out = NULL;
            c->tbl_ncell++;
            c->tbl_col++;
            c->tbl_in_cell = 0;
        }
        break;

    case MD_BLOCK_TABLE: {
        int ncols = (int)c->tbl_cols;
        int colw[32];
        int row_lines[MAX_TROWS];

        tbl_calc_colw(c, ncols, colw);
        tbl_wrap_cells(c, ncols, colw);
        tbl_row_lines(c, row_lines);
        int header_rows = tbl_count_headers(c);

        /* ── If table height exceeds 80% of terminal height,
         *     expand columns to full terminal width to reduce
         *     vertical wrapping and keep the table from growing
         *     beyond the terminal scrollback. ── */
        int term_h = md_get_terminal_height();
        if (term_h > 0 && c->max_width > 0) {
            int total_height = 0;
            for (int ri = 0; ri < c->tbl_nrow; ri++)
                total_height += row_lines[ri];
            /* borders: top (1) + separator lines (nrows-1) + bottom (1) */
            total_height += c->tbl_nrow + 1;

            if (total_height > term_h * 80 / 100) {
                /* Recalculate column widths using full available width.
                 * Distribute evenly among columns. */
                int borders = ncols + 1;
                int avail = c->max_width - borders;
                int base = avail / ncols;
                int rem  = avail % ncols;
                for (int i = 0; i < ncols; i++) {
                    colw[i] = base + (i < rem ? 1 : 0);
                    if (colw[i] < MIN_COL_WIDTH) colw[i] = MIN_COL_WIDTH;
                }
                /* Re-wrap cells and recompute row heights */
                tbl_wrap_cells(c, ncols, colw);
                tbl_row_lines(c, row_lines);
            }
        }

        tbl_render(c, ncols, colw, row_lines, header_rows);
        tbl_free_cells(c);

        out_char(c, '\n');
        c->bol = 1;
        break;
    }

    default:
        break;
    }
    return 0;
}

static int enter_span_cb(MD_SPANTYPE type, void *detail, void *userdata) {
    mdrenderer_ctx *c = (mdrenderer_ctx *)userdata;

    if (c->style_depth < MAX_STYLES)
        c->styles[c->style_depth++] = type;

    if (type == MD_SPAN_IMG) {
        MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
        handle_bol_prefix(c);
        out_str(c, ANSI_DIM ANSI_YELLOW "[IMG:");
        if (d->src.size > 0) {
            out_char(c, ' ');
            out_raw(c, d->src.text, d->src.size);
        }
        out_str(c, "]");
        restyle(c);
        return 0;
    }

    restyle(c);
    if (type == MD_SPAN_CODE) {
        handle_bol_prefix(c);
        out_str(c, "`");
    }
    return 0;
}

static int leave_span_cb(MD_SPANTYPE type, void *detail, void *userdata) {
    mdrenderer_ctx *c = (mdrenderer_ctx *)userdata;

    if (type == MD_SPAN_A) {
        MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
        if (d->href.size > 0) {
            handle_bol_prefix(c);
            out_str(c, ANSI_DIM " <");
            out_raw(c, d->href.text, d->href.size);
            out_str(c, ">" ANSI_RESET);
        }
    }

    if (type == MD_SPAN_CODE) {
        handle_bol_prefix(c);
        out_str(c, "`");
    }

    if (c->style_depth > 0)
        c->style_depth--;

    restyle(c);
    return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) {
    mdrenderer_ctx *c = (mdrenderer_ctx *)userdata;

    switch (type) {
    case MD_TEXT_NORMAL:
        emit_text(c, text, size);
        break;

    case MD_TEXT_NULLCHAR:
        emit_text(c, "\xef\xbf\xbd", 3);
        break;

    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
        out_str(c, ANSI_RESET "\n");
        c->bol = 1;
        break;

    case MD_TEXT_ENTITY:
        out_raw(c, text, size);
        break;

    case MD_TEXT_CODE:
        if (c->in_code_block) {
            out_raw(c, text, size);
        } else {
            emit_text(c, text, size);
        }
        break;

    case MD_TEXT_HTML:
        out_str(c, ANSI_DIM);
        out_raw(c, text, size);
        out_str(c, ANSI_RESET);
        break;

    case MD_TEXT_LATEXMATH:
        out_str(c, ANSI_MAGENTA);
        out_raw(c, text, size);
        out_str(c, ANSI_RESET);
        break;

    default:
        emit_text(c, text, size);
        break;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * Core: parse markdown into an sds ANSI-output buffer
 * ════════════════════════════════════════════════════════════════ */

sds render_markdown(Arena *a, const char *input, size_t len, int max_width) {
    mdrenderer_ctx c;
    memset(&c, 0, sizeof(c));
    c.arena = a;
    c.bol = 1;
    c.max_width = max_width;
    c.out = sdsempty(a);

    MD_PARSER parser;
    memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = MD_FLAG_COLLAPSEWHITESPACE |
                   MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_TABLES |
                   MD_FLAG_STRIKETHROUGH |
                   MD_FLAG_TASKLISTS |
                   MD_FLAG_UNDERLINE |
                   MD_FLAG_SUPERSCRIPTS |
                   MD_FLAG_SUBSCRIPTS;
    parser.enter_block = enter_block_cb;
    parser.leave_block = leave_block_cb;
    parser.enter_span = enter_span_cb;
    parser.leave_span = leave_span_cb;
    parser.text = text_cb;

    /* Note: sanitize_utf8 is NOT called here because in streaming mode,
     * the buffer is incrementally accumulated and a partial multi-byte
     * sequence at a chunk boundary would be corrupted. Pipe input is
     * assumed to be valid UTF-8. */
    md_parse(input, (MD_SIZE)len, &parser, &c);
    out_str(&c, ANSI_RESET);

    return c.out;
}

/* ── wrap ANSI-rendered output so every \n separated line fits within max_width ── */
/* Each input "logical line" (delimited by \n) is wrapped using
 * wrap_styled_text() so that its visible width ≤ max_width.
 * The result preserves all ANSI codes, with proper reset at each
 * continuation line boundary. This guarantees that \n count == terminal rows. */
sds wrap_rendered_to_width(Arena *a, const char *rendered, size_t len, int max_width) {
    if (max_width < 1 || len == 0)
        return sdsnewlen(a, rendered, len);

    sds result = sdsempty(a);
    /* Track ANSI state carried across \n boundaries.
     * E.g. code blocks: \033[92m is emitted once before the block,
     * and all subsequent lines must inherit the green.*/
    char carry_ansi[512] = {0};
    int carry_len = 0;
    size_t i = 0;
    int first_line = 1;

    while (i < len) {
        size_t line_start = i;
        while (i < len && rendered[i] != '\n') i++;
        size_t seg_len = i - line_start;

        if (seg_len > 0) {
            /* Build segment: carried ANSI state + original content */
            size_t total = (size_t)carry_len + seg_len;
            char *seg = (char *)arena_alloc(a, total + 1);
            if (carry_len > 0)
                memcpy(seg, carry_ansi, (size_t)carry_len);
            memcpy(seg + carry_len, rendered + line_start, seg_len);
            seg[total] = '\0';

            int nw;
            char **wrapped = wrap_styled_text(a, seg, max_width, &nw);
            for (int j = 0; j < nw; j++) {
                if (!first_line) result = sdscatlen(result, "\n", 1);
                result = sdscat(result, wrapped[j]);
                first_line = 0;
            }

            /* Update carried ANSI state from this segment:
             * scan for non-reset codes to carry forward;
             * reset clears the carry buffer. */
            for (size_t si = 0; si < seg_len;) {
                if (rendered[line_start + si] == '\033') {
                    size_t ss = line_start + si;
                    while (si < seg_len && rendered[line_start + si] != 'm') si++;
                    if (si < seg_len) si++;
                    int sl = (int)(si - (ss - line_start));
                    if (sl >= 3 && rendered[ss + 2] == '0' &&
                        (sl == 4 || rendered[ss + 3] == 'm')) {
                        carry_len = 0;
                        carry_ansi[0] = '\0';
                    } else if (carry_len + sl < (int)sizeof(carry_ansi) - 1) {
                        memcpy(carry_ansi + carry_len, rendered + ss, (size_t)sl);
                        carry_len += sl;
                        carry_ansi[carry_len] = '\0';
                    }
                } else {
                    si++;
                }
            }
        } else {
            /* empty logical line: preserve as-is */
            if (!first_line) result = sdscatlen(result, "\n", 1);
            first_line = 0;
        }

        if (i < len && rendered[i] == '\n') {
            i++; /* skip the original \n */
        }
    }
    return result;
}

/* ── line array helpers (for diff-based rendering) ──────────── */

typedef struct {
    const char *start;
    size_t len;
} sds_line;

/* Split an sds string into an array of line pointers. Each line
 * does NOT include the \n separator. *nlines receives the count. */
sds_line *sds_split_lines(Arena *a, const char *s, size_t slen, int *nlines) {
    if (slen == 0) { *nlines = 0; return NULL; }
    int cap = 16;
    sds_line *lines = (sds_line *)arena_alloc(a, (size_t)cap * sizeof(sds_line));
    *nlines = 0;
    size_t pos = 0;
    while (pos < slen) {
        const char *nl = memchr(s + pos, '\n', slen - pos);
        size_t seg = nl ? (size_t)(nl - (s + pos)) : slen - pos;
        if (*nlines >= cap) {
            size_t old = (size_t)cap * sizeof(sds_line);
            cap *= 2;
            lines = (sds_line *)arena_realloc(a, lines, old, (size_t)cap * sizeof(sds_line));
        }
        lines[*nlines].start = s + pos;
        lines[*nlines].len = seg;
        (*nlines)++;
        pos += seg;
        if (nl) pos++;  /* skip the \n */
    }
    return lines;
}

int line_eq(const sds_line *a, const sds_line *b) {
    return a->len == b->len && memcmp(a->start, b->start, a->len) == 0;
}

int md_get_terminal_width(void) {
    struct winsize ws;
    int fds[] = {STDOUT_FILENO, STDIN_FILENO, STDERR_FILENO};
    for (int i = 0; i < 3; i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            return (int)ws.ws_col;
    }
    return 80;
}

/* Get terminal height (number of rows) from TIOCGWINSZ, or fall back to 24. */
static int md_get_terminal_height(void) {
    struct winsize ws;
    int fds[] = {STDOUT_FILENO, STDIN_FILENO, STDERR_FILENO};
    for (int i = 0; i < 3; i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
            return (int)ws.ws_row;
    }
    return 24;
}

/* ════════════════════════════════════════════════════════════════
 * Incremental renderer API
 * ════════════════════════════════════════════════════════════════ */

struct md_renderer {
    Arena  arena;    /* arena for rendered output (reset each feed) */
    char  *raw_buf;  /* accumulated raw markdown (malloc'd) */
    size_t raw_len;  /* byte length of raw_buf */
    int    max_width;
};

md_renderer_t *md_renderer_new(int max_width) {
    md_renderer_t *r = malloc(sizeof(*r));
    memset(r, 0, sizeof(*r));
    r->max_width = max_width;
    return r;
}

void md_renderer_free(md_renderer_t *r) {
    if (!r) return;
    arena_free(&r->arena);
    free(r->raw_buf);
    free(r);
}

void md_renderer_reset(md_renderer_t *r) {
    if (!r) return;
    arena_reset(&r->arena);
    arena_trim(&r->arena);
    free(r->raw_buf);
    r->raw_buf = NULL;
    r->raw_len = 0;
}

sds md_renderer_feed(md_renderer_t *r, const char *text, int len) {
    if (!r || !text || len <= 0) return sdsempty(&r->arena);

    /* Append to raw buffer (plain malloc, not arena) */
    size_t new_len = r->raw_len + (size_t)len;
    char *new_buf = realloc(r->raw_buf, new_len + 1);
    if (!new_buf) return sdsempty(&r->arena);  /* OOM: raw unchanged */
    memcpy(new_buf + r->raw_len, text, (size_t)len);
    new_buf[new_len] = '\0';
    r->raw_buf = new_buf;
    r->raw_len = new_len;

    /* Determine effective width */
    int w = r->max_width;
    if (w <= 0 && isatty(fileno(stdout)))
        w = md_get_terminal_width();
    if (w > 0 && w < MIN_COL_WIDTH) w = MIN_COL_WIDTH;

    /* Reset arena so each feed only holds the *current* rendered output,
     * avoiding O(N²) growth from accumulated historical renders. */
    arena_reset(&r->arena);
    arena_trim(&r->arena);

    /* Parse and render from scratch */
    sds rendered = render_markdown(&r->arena, r->raw_buf, r->raw_len, w);

    /* Wrap to terminal width so \n count == row count */
    if (w > 0) {
        sds wrapped = wrap_rendered_to_width(&r->arena, rendered, sdslen(rendered), w);
        return wrapped;
    }
    return rendered;
}

sds md_renderer_output(md_renderer_t *r) {
    if (!r) return NULL;
    int w = r->max_width;
    if (w <= 0 && isatty(fileno(stdout)))
        w = md_get_terminal_width();
    if (w > 0 && w < MIN_COL_WIDTH) w = MIN_COL_WIDTH;
    arena_reset(&r->arena);
    arena_trim(&r->arena);
    sds rendered = render_markdown(&r->arena, r->raw_buf, r->raw_len, w);
    if (w > 0) {
        sds wrapped = wrap_rendered_to_width(&r->arena, rendered, sdslen(rendered), w);
        return wrapped;
    }
    return rendered;
}

sds md_renderer_raw(md_renderer_t *r) {
    return r ? r->raw_buf : NULL;
}

/* ── Diff helpers ─────────────────────────────────────────── */

int md_count_lines(const char *s) {
    if (!s) return 0;
    int n = 0;
    for (const char *p = s; *p; p++)
        if (*p == '\n') n++;
    return n;
}

int md_compute_diff(const char *old_text, int old_len,
                    const char *new_text, int new_len) {
    Arena da = {0};
    int old_nl, new_nl;
    sds_line *old_lines = sds_split_lines(&da, old_text, (size_t)old_len, &old_nl);
    sds_line *new_lines = sds_split_lines(&da, new_text, (size_t)new_len, &new_nl);

    int first_changed = 0;
    while (first_changed < old_nl && first_changed < new_nl &&
           line_eq(&old_lines[first_changed], &new_lines[first_changed]))
        first_changed++;

    if (first_changed >= old_nl && first_changed >= new_nl)
        first_changed = -1;  /* identical */

    arena_free(&da);
    return first_changed;
}

void md_print_diff(const char *old_text, int old_lines,
                   const char *new_text, int new_lines,
                   int first_changed) {
    if (first_changed < 0) return;  /* no change */

    Arena da = {0};

    int old_nl, new_nl;
    sds_split_lines(&da, old_text,
                    old_text ? strlen(old_text) : 0, &old_nl);
    sds_line *new_l = sds_split_lines(&da, new_text,
                                       new_text ? strlen(new_text) : 0, &new_nl);

    /* ── Begin synchronized output (CSI 2026) ── */
    printf("\033[?2026h");

    int append_only = (first_changed == old_nl && new_nl > old_nl);

    if (append_only) {
        sds app = sdsempty(&da);
        for (int i = first_changed; i < new_nl; i++) {
            app = sdscatlen(app, "\n", 1);
            app = sdscatlen(app, new_l[i].start, new_l[i].len);
        }
        fwrite(app, 1, sdslen(app), stdout);
    } else {
        int back = old_lines - first_changed;
        if (back > 0) printf("\033[%dA", back);
        printf("\r\033[J");

        sds repl = sdsempty(&da);
        for (int i = first_changed; i < new_nl; i++) {
            if (i > first_changed) repl = sdscatlen(repl, "\n", 1);
            repl = sdscatlen(repl, new_l[i].start, new_l[i].len);
        }
        if (new_lines > 0)
            repl = sdscatlen(repl, "\n", 1);
        fwrite(repl, 1, sdslen(repl), stdout);

        if (new_nl < old_nl) {
            int extra = old_nl - new_nl;
            for (int i = 0; i < extra; i++)
                printf("\r\n\033[2K");
            printf("\033[%dA", extra);
        }
    }

    printf("\033[?2026l");
    fflush(stdout);

    arena_free(&da);
}

/* ── High-level display helper ────────────────────────────── */

void md_display_init(md_display_t *d, int max_width) {
    memset(d, 0, sizeof(*d));
    d->r = md_renderer_new(max_width);
}

void md_display_free(md_display_t *d) {
    if (!d) return;
    md_renderer_free(d->r);
    free(d->prev);
    memset(d, 0, sizeof(*d));
}

void md_display_reset(md_display_t *d) {
    if (!d) return;
    md_renderer_reset(d->r);
    free(d->prev);
    d->prev = NULL;
    d->prev_len = 0;
    d->prev_lines = 0;
}

void md_display_feed(md_display_t *d, const char *text, int len) {
    if (!d || !text || len <= 0) return;

    sds cur = md_renderer_feed(d->r, text, len);
    int cur_lines = md_count_lines(cur);
    int cur_len   = (int)sdslen(cur);

    if (!d->prev) {
        /* First chunk: output directly */
        fwrite(cur, 1, (size_t)cur_len, stdout);
        fflush(stdout);
    } else {
        /* Diff-based in-place update */
        int first = md_compute_diff(d->prev, d->prev_len,
                                     cur, cur_len);
        if (first >= 0) {
            md_print_diff(d->prev, d->prev_lines, cur, cur_lines, first);
        }
    }

    /* Save current as prev for next diff (malloc copy — cur lives in
     * the renderer arena and will be invalidated on the next feed). */
    free(d->prev);
    d->prev = malloc((size_t)cur_len + 1);
    if (d->prev) {
        memcpy(d->prev, cur, (size_t)cur_len + 1);
        d->prev_len   = cur_len;
        d->prev_lines = cur_lines;
    } else {
        d->prev = NULL;
        d->prev_len   = 0;
        d->prev_lines = 0;
    }
}
