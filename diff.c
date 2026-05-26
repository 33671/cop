/*
 * diff.c
 *
 * Standalone unified-diff generator based on Harold Stone's longest-
 * common-subsequence algorithm.
 */

#define _GNU_SOURCE   /* for open_memstream */
#include "diff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>

/* ─── helpers ─────────────────────────────────────────────────────────── */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) abort();
    return p;
}
static void *xzalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) abort();
    return p;
}
static void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) abort();
    return p;
}

/* integer square root */
static unsigned isqrt(unsigned N) {
    if (N == 0) return 0;
    unsigned s = 1;
    while (s * s <= N) s++;
    return s - 1;
}

/* ─── file-position tracking ──────────────────────────────────────────── */

typedef struct {
    FILE *fp;
    long  pos;
} fp_pos_t;

static void seek_ft(fp_pos_t *ft, long pos) {
    if (ft->pos != pos) {
        ft->pos = pos;
        fseek(ft->fp, pos, SEEK_SET);
    }
}

/* ─── token types ─────────────────────────────────────────────────────── */

typedef int token_t;

enum {
    TOK_EMPTY = 1 << 9,
    TOK_EOF   = 1 << 10,
    TOK_EOL   = 1 << 11,
    CHAR_MASK = 0x1ff,
};

#define TOK2CHAR(t) ((t) & CHAR_MASK)

/*
 * Read one token from fp.  Since we never use -b, -w, or -i this
 * is straightforward: one character at a time, flag EOL / EOF.
 */
static int read_token(fp_pos_t *ft, token_t tok) {
    tok |= TOK_EMPTY;
    while (!(tok & TOK_EOL)) {
        int t = fgetc(ft->fp);
        if (t != EOF)
            ft->pos++;
        tok |= (t & (TOK_EOF + TOK_EOL));
        if (t == '\n')
            tok |= TOK_EOL;
        t &= CHAR_MASK;
        tok &= ~(TOK_EMPTY + CHAR_MASK);
        tok |= t;
        break;
    }
    return tok;
}

/* ─── Stone LCS algorithm ─────────────────────────────────────────────── */

struct cand { int x, y, pred; };

static int search(const int *c, int k, int y, const struct cand *list) {
    if (list[c[k]].y < y)
        return k + 1;
    int i, j;
    for (i = 0, j = k + 1;;) {
        int l = (i + j) >> 1;
        if (l > i) {
            int t = list[c[l]].y;
            if (t > y)       j = l;
            else if (t < y)  i = l;
            else             return l;
        } else {
            return l + 1;
        }
    }
}

static void stone(const int *a, int n, const int *b, int *J, int pref) {
    unsigned sq    = isqrt(n);
    unsigned bound = MAX(256, sq);
    int clen       = 1;
    int clistlen   = 100;
    int k          = 0;
    struct cand *clist = xzalloc((size_t)clistlen * sizeof(clist[0]));
    struct cand cand;
    int *klist = xzalloc(((size_t)n + 2) * sizeof(klist[0]));

    for (cand.x = 1; cand.x <= n; cand.x++) {
        int j = a[cand.x], oldl = 0;
        unsigned numtries = 0;
        if (j == 0)
            continue;
        cand.y = -b[j];
        cand.pred = klist[0];
        do {
            int l, tc;
            if (cand.y <= clist[cand.pred].y)
                continue;
            l = search(klist, k, cand.y, clist);
            if (l != oldl + 1)
                cand.pred = klist[l - 1];
            if (l <= k && clist[klist[l]].y <= cand.y)
                continue;
            if (clen == clistlen) {
                clistlen = clistlen * 11 / 10;
                clist = xrealloc(clist, (size_t)clistlen * sizeof(clist[0]));
            }
            clist[clen] = cand;
            tc = klist[l];
            klist[l] = clen++;
            if (l <= k) {
                cand.pred = tc;
                oldl = l;
                numtries++;
            } else {
                k++;
                break;
            }
        } while ((cand.y = b[++j]) > 0 && numtries < bound);
    }
    /* unravel */
    struct cand *q;
    for (q = clist + klist[k]; q->y; q = clist + q->pred)
        J[q->x + pref] = q->y + pref;

    free(klist);
    free(clist);
}

/* ─── line hashing / sorting ──────────────────────────────────────────── */

struct line {
    union {
        unsigned serial;
        long     offset;
    };
    unsigned value;
};

static void equiv(struct line *a, int n, struct line *b, int m, int *c) {
    int i = 1, j = 1;
    while (i <= n && j <= m) {
        if      (a[i].value < b[j].value) a[i++].value = 0;
        else if (a[i].value == b[j].value) a[i++].value = (unsigned)j;
        else    j++;
    }
    while (i <= n)
        a[i++].value = 0;
    b[m + 1].value = 0;
    j = 0;
    while (++j <= m) {
        c[j] = -b[j].serial;
        while (b[j + 1].value == b[j].value) {
            j++;
            c[j] = b[j].serial;
        }
    }
    c[j] = -1;
}

static void unsort(const struct line *f, int l, int *b) {
    int i;
    int *a = xmalloc(((size_t)l + 1) * sizeof(a[0]));
    for (i = 1; i <= l; i++)
        a[f[i].serial] = f[i].value;
    for (i = 1; i <= l; i++)
        b[i] = a[i];
    free(a);
}

static int line_compar(const void *a, const void *b) {
    const struct line *l0 = (const struct line *)a;
    const struct line *l1 = (const struct line *)b;
    int r = (int)(l0->value - l1->value);
    if (r) return r;
    return (int)(l0->serial - l1->serial);
}

/* ─── fetch lines for output ──────────────────────────────────────────── */

static void fetch(fp_pos_t *ft, const long *ix, int a, int b,
                  int ch, FILE *out) {
    int i, j, col;
    for (i = a; i <= b; i++) {
        seek_ft(ft, ix[i - 1]);
        fprintf(out, "%c%d: ", ch, i);
        for (j = 0, col = 0; j < ix[i] - ix[i - 1]; j++) {
            int c = fgetc(ft->fp);
            if (c == EOF) {
                fputs("\n\\ No newline at end of file\n", out);
                return;
            }
            ft->pos++;
            if (c == '\t')
                do { fputc(' ', out); } while (++col & 7);
            else {
                fputc(c, out);
                col++;
            }
        }
    }
}

/* ─── create J vector ─────────────────────────────────────────────────── */

static int *create_J(fp_pos_t ft[2], int nlen[2], long *ix[2]) {
    int      *J, slen[2], *class, *member;
    struct line *nfile[2], *sfile[2];
    int pref = 0, suff = 0, i, j;

    /* read & hash lines for both files */
    for (i = 0; i < 2; i++) {
        unsigned hash = 0;
        token_t tok   = 0;
        size_t sz     = 100;

        nfile[i] = xmalloc((sz + 3) * sizeof(nfile[i][0]));
        ft[i].pos = 0;
        fseek(ft[i].fp, 0, SEEK_SET);

        nlen[i] = 0;
        nfile[i][0].offset = 0;

        while (1) {
            tok = read_token(&ft[i], tok);
            if (!(tok & TOK_EMPTY)) {
                /* hash the character */
                unsigned o = hash - TOK2CHAR(tok);
                hash = hash * 128 - o;
                continue;
            }
            /* end of line */
            if ((size_t)nlen[i]++ == sz) {
                sz = sz * 3 / 2;
                nfile[i] = xrealloc(nfile[i],
                                    (sz + 3) * sizeof(nfile[i][0]));
            }
            nfile[i][nlen[i]].value  = hash & INT_MAX;
            nfile[i][nlen[i]].offset = ft[i].pos;
            if (tok & TOK_EOF) {
                nfile[i][nlen[i]].offset++;
                break;
            }
            hash = 0;
            tok  = 0;
        }

        /* drop the lone-EOF sentinel line */
        if (nfile[i][nlen[i]].offset - nfile[i][nlen[i] - 1].offset == 1)
            nlen[i]--;

        ix[i] = xmalloc(((size_t)nlen[i] + 2) * sizeof(ix[i][0]));
        for (j = 0; j < nlen[i] + 1; j++)
            ix[i][j] = nfile[i][j].offset;
    }

    /* common prefix / suffix */
    for (; pref < nlen[0] && pref < nlen[1] &&
           nfile[0][pref + 1].value == nfile[1][pref + 1].value;
         pref++);
    for (; suff < nlen[0] - pref && suff < nlen[1] - pref &&
           nfile[0][nlen[0] - suff].value == nfile[1][nlen[1] - suff].value;
         suff++);

    for (j = 0; j < 2; j++) {
        sfile[j] = nfile[j] + pref;
        slen[j]  = nlen[j] - pref - suff;
        for (i = 0; i <= slen[j]; i++)
            sfile[j][i].serial = (unsigned)i;
        qsort(sfile[j] + 1, (size_t)slen[j],
              sizeof(*sfile[j]), line_compar);
    }

    member = (int *)nfile[1];
    equiv(sfile[0], slen[0], sfile[1], slen[1], member);
    member = xrealloc(member, ((size_t)slen[1] + 2) * sizeof(member[0]));

    class = (int *)nfile[0];
    unsort(sfile[0], slen[0], (int *)nfile[0]);
    class = xrealloc(class, ((size_t)slen[0] + 2) * sizeof(class[0]));

    J = xmalloc(((size_t)nlen[0] + 2) * sizeof(J[0]));
    int delta = nlen[1] - nlen[0];
    for (i = 0; i <= nlen[0]; i++)
        J[i] = i <= pref            ? i
             : i > (nlen[0] - suff) ? (i + delta)
             : 0;

    stone(class, slen[0], member, J, pref);
    J[nlen[0] + 1] = nlen[1] + 1;

    free(class);
    free(member);

    /* verify hashed matches against actual content */
    for (i = 1; i <= nlen[0]; i++) {
        if (!J[i]) continue;
        seek_ft(&ft[0], ix[0][i - 1]);
        seek_ft(&ft[1], ix[1][J[i] - 1]);
        for (j = J[i]; i <= nlen[0] && J[i] == j; i++, j++) {
            token_t tok0 = 0, tok1 = 0;
            do {
                tok0 = read_token(&ft[0], tok0);
                tok1 = read_token(&ft[1], tok1);
                if (((tok0 ^ tok1) & TOK_EMPTY) != 0 /* one empty */
                    || (!(tok0 & TOK_EMPTY)
                        && TOK2CHAR(tok0) != TOK2CHAR(tok1)))
                    J[i] = 0;
            } while (!(tok0 & tok1 & TOK_EMPTY));
        }
    }
    return J;
}

/* ─── unified-diff output ─────────────────────────────────────────────── */

typedef struct { int a, b; } range_t;

static int diff_output(fp_pos_t ft[2], int nlen[2], long *ix[2],
                       int *J, int context, FILE *out) {
    typedef range_t hunk_t[2];  /* [0]=old-range [1]=new-range */
    hunk_t *vec  = NULL;
    int vec_cap  = 0;
    int i        = 1;
    int idx      = -1;
    int anychange = 0;

    do {
        while (1) {
            hunk_t v;
            /* walk unchanged prefix */
            for (v[0].a = i;
                 v[0].a <= nlen[0] && J[v[0].a] == J[v[0].a - 1] + 1;
                 v[0].a++);
            v[1].a = J[v[0].a - 1] + 1;
            /* walk changed region */
            for (v[0].b = v[0].a - 1;
                 v[0].b < nlen[0] && !J[v[0].b + 1];
                 v[0].b++);
            v[1].b = J[v[0].b + 1] - 1;

            if (v[0].a <= v[0].b || v[1].a <= v[1].b) {
                int ct = (2 * context) + 1;
                /* start a new hunk if far enough from previous */
                if (idx >= 0
                    && v[0].a > vec[idx][0].b + ct
                    && v[1].a > vec[idx][1].b + ct)
                    break;

                if (idx + 1 >= vec_cap) {
                    vec_cap = vec_cap ? vec_cap * 2 : 16;
                    vec = realloc(vec,
                                  (size_t)vec_cap * sizeof(vec[0]));
                    if (!vec) abort();
                }
                idx++;
                memcpy(vec[idx], v, sizeof(v));
            }

            i = v[0].b + 1;
            if (i > nlen[0]) break;
            J[v[0].b] = v[1].b;
        }
        if (idx < 0)
            goto cont;

        fprintf(out, "@@");
        for (int j = 0; j < 2; j++) {
            int a = MAX(1, vec[0][j].a - context);
            int b = MIN(nlen[j], vec[idx][j].b + context);
            fprintf(out, " %c%d", j ? '+' : '-', MIN(a, b));
            if (a != b)
                fprintf(out, ",%d", (a < b) ? b - a + 1 : 0);
        }
        fputs(" @@\n", out);

        /* unified context + changes */
        hunk_t span;
        for (int j = 0; j < 2; j++) {
            span[j].a = MAX(1, vec[0][j].a - context);
            span[j].b = MIN(nlen[j], vec[idx][j].b + context);
        }

        int cv = 0;
        int lowa = span[0].a;
        for (;;) {
            int end = (cv > idx);
            fetch(&ft[0], ix[0], lowa,
                  end ? span[0].b : vec[cv][0].a - 1, ' ', out);
            if (end) break;
            fetch(&ft[0], ix[0], vec[cv][0].a, vec[cv][0].b, '-', out);
            fetch(&ft[1], ix[1], vec[cv][1].a, vec[cv][1].b, '+', out);
            lowa = vec[cv][0].b + 1;
            cv++;
        }
        anychange = 1;
cont:
        idx = -1;
    } while (i <= nlen[0]);

    free(vec);
    return anychange;
}

/* ─── public API ──────────────────────────────────────────────────────── */

char *diff_text(const char *old_text, size_t old_len,
                const char *new_text, size_t new_len,
                int context_lines) {
    if (!old_text || !new_text)
        return NULL;

    /* write inputs to temp files for proper seeking */
    FILE *fp[2] = { NULL, NULL };
    fp[0] = tmpfile();
    fp[1] = tmpfile();
    if (!fp[0] || !fp[1]) {
        if (fp[0]) fclose(fp[0]);
        if (fp[1]) fclose(fp[1]);
        return NULL;
    }

    if (fwrite(old_text, 1, old_len, fp[0]) != old_len
        || fwrite(new_text, 1, new_len, fp[1]) != new_len) {
        fclose(fp[0]);
        fclose(fp[1]);
        return NULL;
    }
    rewind(fp[0]);
    rewind(fp[1]);

    fp_pos_t ft[2];
    ft[0].fp  = fp[0];  ft[0].pos = 0;
    ft[1].fp  = fp[1];  ft[1].pos = 0;

    int  nlen[2];
    long *ix[2];
    int *J = create_J(ft, nlen, ix);

    /* capture diff output into a string */
    char   *out_buf  = NULL;
    size_t  out_size = 0;
    FILE   *memf     = open_memstream(&out_buf, &out_size);
    if (!memf) {
        free(J);
        free(ix[0]);
        free(ix[1]);
        fclose(fp[0]);
        fclose(fp[1]);
        return NULL;
    }

    int changed = diff_output(ft, nlen, ix, J, context_lines, memf);

    fclose(memf);   /* flushes and sets out_buf / out_size */
    free(J);
    free(ix[0]);
    free(ix[1]);
    fclose(fp[0]);
    fclose(fp[1]);

    if (!changed) {
        free(out_buf);
        return NULL;
    }
    return out_buf;
}
