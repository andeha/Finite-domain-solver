#include "csp.h"
#include "trail.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* =====================================================================
 * Domain: a Briggs sparse set over an integer range [min, min+range).
 *
 *   values[0..size)                -- the dense list of present values
 *                                    (stored as offsets from `min`).
 *   pos[off]                       -- inverse: where in values that
 *                                    offset lives. Valid iff
 *                                    pos[off] < size && values[pos[off]] == off.
 *
 * size and the two arrays are mutated through the trail, so removal
 * and assignment are reversible in O(1).
 * ===================================================================== */

typedef struct {
    uint32_t *values;
    uint32_t *pos;
    uint32_t  size;
    uint32_t  range;
    int32_t   min;
} dom_t;

static void dom_init(dom_t *d, int32_t min, int32_t max) {
    assert(max >= min);
    d->min   = min;
    d->range = (uint32_t)(max - min) + 1u;
    d->size  = d->range;
    d->values = malloc(d->range * sizeof *d->values);
    d->pos    = malloc(d->range * sizeof *d->pos);
    if (!d->values || !d->pos) abort();
    for (uint32_t i = 0; i < d->range; i++) {
        d->values[i] = i;
        d->pos[i]    = i;
    }
}

static void dom_free(dom_t *d) {
    free(d->values);
    free(d->pos);
}

static int dom_contains(const dom_t *d, int32_t v) {
    if (v < d->min) return 0;
    uint32_t off = (uint32_t)(v - d->min);
    if (off >= d->range) return 0;
    return d->pos[off] < d->size && d->values[d->pos[off]] == off;
}

/* Remove `v` from the domain. Returns 1 on actual removal, 0 if absent. */
static int dom_remove(dom_t *d, trail_t *t, int32_t v) {
    if (!dom_contains(d, v)) return 0;
    uint32_t off  = (uint32_t)(v - d->min);
    uint32_t idx  = d->pos[off];
    uint32_t last = d->values[d->size - 1];

    /* Trail every word we are about to overwrite. The size goes last so
     * that trail_restore replays size first (LIFO), keeping intermediate
     * states well-formed for any debugging that walks the structure. */
    trail_save(t, &d->values[idx]);
    trail_save(t, &d->pos[last]);
    trail_save(t, &d->size);

    d->size--;
    d->values[idx] = last;
    d->pos[last]   = idx;
    return 1;
}

static int32_t dom_value_at(const dom_t *d, uint32_t i) {
    return d->min + (int32_t)d->values[i];
}

/* =====================================================================
 * Constraints
 * ===================================================================== */

typedef enum {
    C_NEQ,           /* x != y                                          */
    C_EQ,            /* x == y                                          */
    C_NEQ_C,         /* x != c                                          */
    C_EQ_C,          /* x == c                                          */
    C_TABLE_OK,      /* (x, y) must appear in `pairs`                   */
    C_TABLE_NO       /* (x, y) must NOT appear in `pairs`               */
} ckind_t;

typedef struct {
    ckind_t  kind;
    var_t    x, y;          /* y unused for unary kinds                 */
    int32_t  c;             /* constant for unary kinds                 */
    int32_t *pairs;         /* flattened [x0,y0,x1,y1,...]; owned       */
    uint32_t n_pairs;
} cstr_t;

struct csp {
    dom_t   *doms;
    uint32_t n_vars, cap_vars;
    cstr_t  *cstrs;
    uint32_t n_cstrs, cap_cstrs;
    trail_t  trail;
};

/* =====================================================================
 * Lifecycle and accessors
 * ===================================================================== */

csp_t *csp_new(void) {
    csp_t *c = calloc(1, sizeof *c);
    if (!c) abort();
    trail_init(&c->trail);
    return c;
}

void csp_free(csp_t *csp) {
    if (!csp) return;
    for (uint32_t i = 0; i < csp->n_vars; i++) dom_free(&csp->doms[i]);
    for (uint32_t i = 0; i < csp->n_cstrs; i++) free(csp->cstrs[i].pairs);
    free(csp->doms);
    free(csp->cstrs);
    trail_free(&csp->trail);
    free(csp);
}

var_t csp_var(csp_t *csp, int32_t min, int32_t max) {
    if (csp->n_vars == csp->cap_vars) {
        csp->cap_vars = csp->cap_vars ? csp->cap_vars * 2 : 8;
        csp->doms = realloc(csp->doms, csp->cap_vars * sizeof *csp->doms);
        if (!csp->doms) abort();
    }
    var_t v = csp->n_vars++;
    dom_init(&csp->doms[v], min, max);
    return v;
}

uint32_t csp_dom_size(const csp_t *csp, var_t v) {
    return csp->doms[v].size;
}

int csp_in_dom(const csp_t *csp, var_t v, int32_t value) {
    return dom_contains(&csp->doms[v], value);
}

int32_t csp_value(const csp_t *csp, var_t v) {
    assert(csp->doms[v].size == 1);
    return dom_value_at(&csp->doms[v], 0);
}

/* =====================================================================
 * Constraint posting
 * ===================================================================== */

static cstr_t *push_cstr(csp_t *csp) {
    if (csp->n_cstrs == csp->cap_cstrs) {
        csp->cap_cstrs = csp->cap_cstrs ? csp->cap_cstrs * 2 : 8;
        csp->cstrs = realloc(csp->cstrs, csp->cap_cstrs * sizeof *csp->cstrs);
        if (!csp->cstrs) abort();
    }
    cstr_t *c = &csp->cstrs[csp->n_cstrs++];
    memset(c, 0, sizeof *c);
    return c;
}

void csp_neq(csp_t *csp, var_t x, var_t y) {
    cstr_t *c = push_cstr(csp);
    c->kind = C_NEQ; c->x = x; c->y = y;
}

void csp_eq(csp_t *csp, var_t x, var_t y) {
    cstr_t *c = push_cstr(csp);
    c->kind = C_EQ; c->x = x; c->y = y;
}

void csp_neq_c(csp_t *csp, var_t x, int32_t k) {
    cstr_t *c = push_cstr(csp);
    c->kind = C_NEQ_C; c->x = x; c->c = k;
}

void csp_eq_c(csp_t *csp, var_t x, int32_t k) {
    cstr_t *c = push_cstr(csp);
    c->kind = C_EQ_C; c->x = x; c->c = k;
}

void csp_alldiff(csp_t *csp, const var_t *vars, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            csp_neq(csp, vars[i], vars[j]);
}

static void post_table(csp_t *csp, ckind_t kind, var_t x, var_t y,
                       const int32_t (*pairs)[2], uint32_t n) {
    cstr_t *c = push_cstr(csp);
    c->kind = kind; c->x = x; c->y = y;
    c->n_pairs = n;
    c->pairs = malloc(2u * n * sizeof *c->pairs);
    if (!c->pairs && n) abort();
    for (uint32_t i = 0; i < n; i++) {
        c->pairs[2u*i + 0] = pairs[i][0];
        c->pairs[2u*i + 1] = pairs[i][1];
    }
}

void csp_allowed_pairs(csp_t *csp, var_t x, var_t y,
                       const int32_t (*pairs)[2], uint32_t n) {
    post_table(csp, C_TABLE_OK, x, y, pairs, n);
}

void csp_forbidden_pairs(csp_t *csp, var_t x, var_t y,
                         const int32_t (*pairs)[2], uint32_t n) {
    post_table(csp, C_TABLE_NO, x, y, pairs, n);
}

/* =====================================================================
 * Propagators
 * ===================================================================== */

static int prop_neq(csp_t *csp, var_t x, var_t y) {
    dom_t *dx = &csp->doms[x], *dy = &csp->doms[y];
    if (dx->size == 1) {
        dom_remove(dy, &csp->trail, dom_value_at(dx, 0));
        if (dy->size == 0) return 0;
    }
    if (dy->size == 1) {
        dom_remove(dx, &csp->trail, dom_value_at(dy, 0));
        if (dx->size == 0) return 0;
    }
    return 1;
}

static int prop_eq(csp_t *csp, var_t x, var_t y) {
    dom_t *dx = &csp->doms[x], *dy = &csp->doms[y];
    /* Two passes: prune dx of values absent from dy, then dy of values
     * absent from dx. Iterate by index because removals are swap-and-pop;
     * advance i only when we keep the current value. */
    for (uint32_t i = 0; i < dx->size; ) {
        int32_t v = dom_value_at(dx, i);
        if (!dom_contains(dy, v)) {
            dom_remove(dx, &csp->trail, v);
            if (dx->size == 0) return 0;
        } else {
            i++;
        }
    }
    for (uint32_t i = 0; i < dy->size; ) {
        int32_t v = dom_value_at(dy, i);
        if (!dom_contains(dx, v)) {
            dom_remove(dy, &csp->trail, v);
            if (dy->size == 0) return 0;
        } else {
            i++;
        }
    }
    return 1;
}

static int prop_neq_c(csp_t *csp, var_t x, int32_t c) {
    dom_t *dx = &csp->doms[x];
    dom_remove(dx, &csp->trail, c);
    return dx->size > 0;
}

static int prop_eq_c(csp_t *csp, var_t x, int32_t c) {
    dom_t *dx = &csp->doms[x];
    if (!dom_contains(dx, c)) return 0;
    /* Remove every other value. Snapshot first because removal mutates. */
    uint32_t n = dx->size;
    int32_t scratch[n];                 /* C99 VLA; n is small in practice */
    for (uint32_t i = 0; i < n; i++) scratch[i] = dom_value_at(dx, i);
    for (uint32_t i = 0; i < n; i++)
        if (scratch[i] != c) dom_remove(dx, &csp->trail, scratch[i]);
    return 1;
}

/* AC-3 over a binary whitelist: each value in dom(x) needs at least one
 * supporting value in dom(y), and vice versa. */
static int prop_table_ok(csp_t *csp, var_t x, var_t y,
                         const int32_t *pairs, uint32_t n_pairs) {
    dom_t *dx = &csp->doms[x], *dy = &csp->doms[y];

    for (uint32_t i = 0; i < dx->size; ) {
        int32_t vx = dom_value_at(dx, i);
        int support = 0;
        for (uint32_t k = 0; k < n_pairs; k++) {
            if (pairs[2u*k] == vx && dom_contains(dy, pairs[2u*k + 1])) {
                support = 1; break;
            }
        }
        if (!support) {
            dom_remove(dx, &csp->trail, vx);
            if (dx->size == 0) return 0;
        } else { i++; }
    }
    for (uint32_t i = 0; i < dy->size; ) {
        int32_t vy = dom_value_at(dy, i);
        int support = 0;
        for (uint32_t k = 0; k < n_pairs; k++) {
            if (pairs[2u*k + 1] == vy && dom_contains(dx, pairs[2u*k])) {
                support = 1; break;
            }
        }
        if (!support) {
            dom_remove(dy, &csp->trail, vy);
            if (dy->size == 0) return 0;
        } else { i++; }
    }
    return 1;
}

/* AC-3 over a binary blacklist: a value in dom(x) is supported iff some
 * value in dom(y) is not forbidden with it. */
static int pair_forbidden(const int32_t *pairs, uint32_t n,
                          int32_t vx, int32_t vy) {
    for (uint32_t k = 0; k < n; k++)
        if (pairs[2u*k] == vx && pairs[2u*k + 1] == vy) return 1;
    return 0;
}

static int prop_table_no(csp_t *csp, var_t x, var_t y,
                         const int32_t *pairs, uint32_t n_pairs) {
    dom_t *dx = &csp->doms[x], *dy = &csp->doms[y];

    for (uint32_t i = 0; i < dx->size; ) {
        int32_t vx = dom_value_at(dx, i);
        int support = 0;
        for (uint32_t j = 0; j < dy->size; j++) {
            if (!pair_forbidden(pairs, n_pairs, vx, dom_value_at(dy, j))) {
                support = 1; break;
            }
        }
        if (!support) {
            dom_remove(dx, &csp->trail, vx);
            if (dx->size == 0) return 0;
        } else { i++; }
    }
    for (uint32_t i = 0; i < dy->size; ) {
        int32_t vy = dom_value_at(dy, i);
        int support = 0;
        for (uint32_t j = 0; j < dx->size; j++) {
            if (!pair_forbidden(pairs, n_pairs, dom_value_at(dx, j), vy)) {
                support = 1; break;
            }
        }
        if (!support) {
            dom_remove(dy, &csp->trail, vy);
            if (dy->size == 0) return 0;
        } else { i++; }
    }
    return 1;
}

static int propagate_one(csp_t *csp, const cstr_t *c) {
    switch (c->kind) {
    case C_NEQ:      return prop_neq     (csp, c->x, c->y);
    case C_EQ:       return prop_eq      (csp, c->x, c->y);
    case C_NEQ_C:    return prop_neq_c   (csp, c->x, c->c);
    case C_EQ_C:     return prop_eq_c    (csp, c->x, c->c);
    case C_TABLE_OK: return prop_table_ok(csp, c->x, c->y, c->pairs, c->n_pairs);
    case C_TABLE_NO: return prop_table_no(csp, c->x, c->y, c->pairs, c->n_pairs);
    }
    return 1;
}

/* Naive AC-3 fixpoint: re-run every constraint until a sweep makes no
 * removals. Trail length is the cheap "anything happened?" probe. A
 * production solver would maintain a queue keyed by recently-changed
 * variables; this stays small and correct. */
static int propagate_all(csp_t *csp) {
    for (;;) {
        size_t before = trail_mark(&csp->trail);
        for (uint32_t i = 0; i < csp->n_cstrs; i++) {
            if (!propagate_one(csp, &csp->cstrs[i])) return 0;
        }
        if (trail_mark(&csp->trail) == before) return 1;
    }
}

/* =====================================================================
 * Search: depth-first, first-fail variable order, sparse-set value order.
 * ===================================================================== */

static int find_branch_var(const csp_t *csp) {
    int best = -1;
    uint32_t best_size = 0;
    for (uint32_t i = 0; i < csp->n_vars; i++) {
        uint32_t s = csp->doms[i].size;
        if (s > 1 && (best < 0 || s < best_size)) {
            best = (int)i;
            best_size = s;
        }
    }
    return best;
}

typedef struct {
    csp_solution_cb cb;
    void           *ud;
    size_t          count;
    int             stop;
} search_t;

static void solve_rec(csp_t *csp, search_t *s) {
    if (s->stop) return;
    if (!propagate_all(csp)) return;

    int v = find_branch_var(csp);
    if (v < 0) {
        s->count++;
        if (s->cb && !s->cb(csp, s->ud)) s->stop = 1;
        return;
    }

    /* Snapshot the present values; iterating during mutation needs this. */
    dom_t *d = &csp->doms[v];
    uint32_t n = d->size;
    int32_t vals[n];
    for (uint32_t i = 0; i < n; i++) vals[i] = dom_value_at(d, i);

    for (uint32_t i = 0; i < n && !s->stop; i++) {
        size_t mark = trail_mark(&csp->trail);
        /* Fix v to vals[i] by removing every other value. */
        for (uint32_t j = 0; j < n; j++)
            if (vals[j] != vals[i])
                dom_remove(d, &csp->trail, vals[j]);
        solve_rec(csp, s);
        trail_restore(&csp->trail, mark);
    }
}

size_t csp_solve(csp_t *csp, csp_solution_cb cb, void *ud) {
    search_t s = { cb, ud, 0, 0 };
    solve_rec(csp, &s);
    return s.count;
}
