/*
 * regalloc.c -- live-variable analysis, def-use chains, and graph-colour
 *               register allocation in one file, on a fixed example.
 *
 * The pipeline is the textbook one from any compilers book:
 *
 *     IR  -->  block use/def  -->  block-level liveness  -->
 *     per-instruction live sets  -->  interference graph  -->
 *     graph k-colouring  -->  virtual-to-physical mapping
 *
 * Liveness is a backwards may-dataflow with the standard
 * iterate-to-fixpoint solver. Sets are stored as Briggs sparse sets
 * (the exact pattern in gcc/sparseset.h), so union / membership /
 * removal are all O(1). Def-use chains are read directly off the IR
 * because every program variable is defined exactly once -- skipping
 * reaching-definitions analysis lets the file stay short. Register
 * allocation drops the interference graph straight into the CSP
 * solver from csp.h: one CSP variable per program variable, domain
 * [0, K-1] for K registers, and a `csp_neq` per interference edge.
 *
 * Build with the existing Makefile:
 *
 *     make regalloc && ./regalloc
 */

#include "csp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * IR
 *
 * Small three-address code. Every instruction has at most one
 * destination and at most two source operands; OP_CONST puts a literal
 * in src1. Branches and returns have no destination.
 * ============================================================================ */

#define MAX_VARS   16
#define MAX_INSNS  32
#define MAX_BLOCKS  8

enum { OP_CONST, OP_ADD, OP_SUB, OP_MUL, OP_BR_GT, OP_RET };

typedef struct {
    int op;
    int dst;        /* -1 if no destination                         */
    int s1, s2;     /* operands; -1 if unused                       */
} insn_t;

typedef struct {
    const char *name;
    int         start, end;     /* half-open [start, end) into INSNS  */
    int         succ[2];        /* up to two successors, -1 if absent */
} block_t;

/* The example -------------------------------------------------------------
 *
 *   entry:
 *     v0 = 10
 *     v1 = 3
 *     v2 = v0 - v1
 *     br_gt v2  -> then / else
 *
 *   then:                       (a > b case: a*a + b)
 *     v3 = v0 * v0
 *     v4 = v3 + v1
 *     ret v4
 *
 *   else:                       (a <= b case: b*b - a)
 *     v5 = v1 * v1
 *     v6 = v5 - v0
 *     ret v6
 *
 * Each variable is defined exactly once. The example is sized so that
 * v0/v1/v2 form a triangle in the interference graph -- forcing at
 * least three registers -- while v3, v4, v5, v6 can share registers
 * with the input variables they don't overlap.
 * ----------------------------------------------------------------------- */

static const insn_t INSNS[] = {
    /* entry */
    /*  0 */ {OP_CONST, 0, 10, -1},
    /*  1 */ {OP_CONST, 1,  3, -1},
    /*  2 */ {OP_SUB,   2,  0,  1},
    /*  3 */ {OP_BR_GT, -1, 2, -1},
    /* then */
    /*  4 */ {OP_MUL,   3,  0,  0},
    /*  5 */ {OP_ADD,   4,  3,  1},
    /*  6 */ {OP_RET,  -1,  4, -1},
    /* else */
    /*  7 */ {OP_MUL,   5,  1,  1},
    /*  8 */ {OP_SUB,   6,  5,  0},
    /*  9 */ {OP_RET,  -1,  6, -1},
};
static const int N_INSNS  = (int)(sizeof INSNS / sizeof INSNS[0]);

static const block_t BLOCKS[] = {
    {"entry", 0,  4, { 1,  2}},
    {"then",  4,  7, {-1, -1}},
    {"else",  7, 10, {-1, -1}},
};
static const int N_BLOCKS = (int)(sizeof BLOCKS / sizeof BLOCKS[0]);

#define N_VARS 7
static const char *VAR[N_VARS] = {"v0","v1","v2","v3","v4","v5","v6"};

/* ----- IR queries -------------------------------------------------------- */

static int insn_defs(int i, int *out) {
    if (INSNS[i].dst >= 0) { *out = INSNS[i].dst; return 1; }
    return 0;
}

/* Fill `out` with the variable operands actually read; returns count.
 * OP_CONST's s1 is a literal, not a variable, so it produces zero uses. */
static int insn_uses(int i, int out[2]) {
    const insn_t *I = &INSNS[i];
    int n = 0;
    switch (I->op) {
    case OP_CONST: break;
    case OP_ADD: case OP_SUB: case OP_MUL:
        if (I->s1 >= 0) out[n++] = I->s1;
        if (I->s2 >= 0) out[n++] = I->s2;
        break;
    case OP_BR_GT: case OP_RET:
        if (I->s1 >= 0) out[n++] = I->s1;
        break;
    }
    return n;
}

/* ============================================================================
 * Sparse set of variable IDs
 *
 * dense[0..n) holds the present elements in arbitrary order;
 * sparse[v] is the position of v in dense (only valid when
 * sparse[v] < n && dense[sparse[v]] == v). Membership, insertion,
 * and removal are all O(1). Iteration walks dense in O(n).
 * ============================================================================ */

typedef struct {
    uint32_t dense [MAX_VARS];
    uint32_t sparse[MAX_VARS];
    uint32_t n;
} vset_t;

static void vset_clear(vset_t *s)               { s->n = 0; }

static int vset_contains(const vset_t *s, uint32_t v) {
    if (v >= MAX_VARS) return 0;
    return s->sparse[v] < s->n && s->dense[s->sparse[v]] == v;
}

static void vset_add(vset_t *s, uint32_t v) {
    if (vset_contains(s, v)) return;
    s->dense[s->n]  = v;
    s->sparse[v]    = s->n;
    s->n++;
}

static void vset_remove(vset_t *s, uint32_t v) {
    if (!vset_contains(s, v)) return;
    uint32_t i    = s->sparse[v];
    uint32_t last = s->dense[s->n - 1];
    s->dense[i]   = last;
    s->sparse[last] = i;
    s->n--;
}

static void vset_union(vset_t *dst, const vset_t *src) {
    for (uint32_t i = 0; i < src->n; i++) vset_add(dst, src->dense[i]);
}

static int vset_eq(const vset_t *a, const vset_t *b) {
    if (a->n != b->n) return 0;
    for (uint32_t i = 0; i < a->n; i++)
        if (!vset_contains(b, a->dense[i])) return 0;
    return 1;
}

static void vset_copy(vset_t *dst, const vset_t *src) { *dst = *src; }

static void vset_print(const vset_t *s) {
    putchar('{');
    /* Sort visually so the output is stable across runs. */
    int seen[MAX_VARS] = {0};
    for (uint32_t i = 0; i < s->n; i++) seen[s->dense[i]] = 1;
    int first = 1;
    for (int v = 0; v < N_VARS; v++) {
        if (!seen[v]) continue;
        if (!first) printf(", ");
        printf("%s", VAR[v]);
        first = 0;
    }
    putchar('}');
}

/* ============================================================================
 * Pretty printers
 * ============================================================================ */

static void print_insn(int i) {
    const insn_t *I = &INSNS[i];
    switch (I->op) {
    case OP_CONST: printf("%-2s = %d",            VAR[I->dst], I->s1); break;
    case OP_ADD:   printf("%-2s = %s + %s",       VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
    case OP_SUB:   printf("%-2s = %s - %s",       VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
    case OP_MUL:   printf("%-2s = %s * %s",       VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
    case OP_BR_GT: printf("br_gt %s",             VAR[I->s1]); break;
    case OP_RET:   printf("ret  %s",              VAR[I->s1]); break;
    }
}

static void print_program(void) {
    for (int b = 0; b < N_BLOCKS; b++) {
        const block_t *B = &BLOCKS[b];
        printf("%s:\n", B->name);
        for (int i = B->start; i < B->end; i++) {
            printf("  %2d   ", i);
            print_insn(i);
            putchar('\n');
        }
        if (B->succ[0] >= 0) {
            printf("        -> %s", BLOCKS[B->succ[0]].name);
            if (B->succ[1] >= 0) printf(", %s", BLOCKS[B->succ[1]].name);
            putchar('\n');
        }
    }
}

/* ============================================================================
 * Live-variable analysis (backwards may-dataflow)
 *
 *     use[B]  = variables read in B before being written in B
 *     def[B]  = variables written anywhere in B
 *     out[B]  = union of in[S] for each successor S
 *     in [B]  = use[B] ∪ (out[B] \ def[B])
 *
 * Iterate to fixpoint. With three blocks this converges in one or two
 * passes; the algorithm scales linearly in (blocks × edges) per pass
 * regardless of which scheduling order is used.
 * ============================================================================ */

static vset_t USE[MAX_BLOCKS], DEF[MAX_BLOCKS];
static vset_t LIVE_IN[MAX_BLOCKS], LIVE_OUT[MAX_BLOCKS];
static vset_t LIVE_AT[MAX_INSNS];     /* live-OUT of each instruction */

static void compute_block_use_def(void) {
    for (int b = 0; b < N_BLOCKS; b++) {
        vset_clear(&USE[b]);
        vset_clear(&DEF[b]);
        for (int i = BLOCKS[b].start; i < BLOCKS[b].end; i++) {
            int uses[2];
            int n = insn_uses(i, uses);
            for (int u = 0; u < n; u++)
                if (!vset_contains(&DEF[b], (uint32_t)uses[u]))
                    vset_add(&USE[b], (uint32_t)uses[u]);
            int d;
            if (insn_defs(i, &d)) vset_add(&DEF[b], (uint32_t)d);
        }
    }
}

static int compute_block_liveness(void) {
    for (int b = 0; b < N_BLOCKS; b++) {
        vset_clear(&LIVE_IN[b]);
        vset_clear(&LIVE_OUT[b]);
    }
    int iter = 0, changed = 1;
    while (changed) {
        changed = 0; iter++;
        /* Reverse order helps for forward CFGs but the fixpoint is
         * order-independent. */
        for (int b = N_BLOCKS - 1; b >= 0; b--) {
            vset_t new_out, new_in;
            vset_clear(&new_out);
            for (int s = 0; s < 2; s++) {
                int succ = BLOCKS[b].succ[s];
                if (succ >= 0) vset_union(&new_out, &LIVE_IN[succ]);
            }
            vset_copy(&new_in, &new_out);
            for (uint32_t k = 0; k < DEF[b].n; k++)
                vset_remove(&new_in, DEF[b].dense[k]);
            vset_union(&new_in, &USE[b]);

            if (!vset_eq(&new_in,  &LIVE_IN[b]) ||
                !vset_eq(&new_out, &LIVE_OUT[b])) {
                changed = 1;
                vset_copy(&LIVE_IN[b],  &new_in);
                vset_copy(&LIVE_OUT[b], &new_out);
            }
        }
    }
    return iter;
}

/* Within each block, walk backwards to derive per-instruction live sets. */
static void compute_insn_liveness(void) {
    for (int b = 0; b < N_BLOCKS; b++) {
        vset_t live;
        vset_copy(&live, &LIVE_OUT[b]);
        for (int i = BLOCKS[b].end - 1; i >= BLOCKS[b].start; i--) {
            vset_copy(&LIVE_AT[i], &live);   /* live-out of instruction i */
            int d;
            if (insn_defs(i, &d)) vset_remove(&live, (uint32_t)d);
            int uses[2];
            int n = insn_uses(i, uses);
            for (int u = 0; u < n; u++) vset_add(&live, (uint32_t)uses[u]);
        }
    }
}

/* ============================================================================
 * Def-use chains
 *
 * One pass per variable: locate its single definition, then collect every
 * instruction that reads it. With multiple definitions per variable this
 * would need reaching-definitions analysis; here it doesn't.
 * ============================================================================ */

static void print_def_use_chains(void) {
    for (int v = 0; v < N_VARS; v++) {
        int def_at = -1;
        for (int i = 0; i < N_INSNS; i++) {
            int d;
            if (insn_defs(i, &d) && d == v) { def_at = i; break; }
        }
        printf("  %s   def @ %2d   used @", VAR[v], def_at);
        int any = 0;
        for (int i = 0; i < N_INSNS; i++) {
            int uses[2], n = insn_uses(i, uses);
            for (int u = 0; u < n; u++) {
                if (uses[u] == v) {
                    printf(" %d", i);
                    any = 1;
                    break;          /* count each instruction at most once */
                }
            }
        }
        if (!any) printf(" (unused)");
        putchar('\n');
    }
}

/* ============================================================================
 * Interference graph (Chaitin)
 *
 * At every program point with a definition d, d interferes with every
 * variable in live-out except itself. Iterating this rule across all
 * instructions captures every simultaneous-liveness pair.
 * ============================================================================ */

static int INTERF[N_VARS][N_VARS];

static void compute_interference(void) {
    memset(INTERF, 0, sizeof INTERF);
    for (int i = 0; i < N_INSNS; i++) {
        int d;
        if (!insn_defs(i, &d)) continue;
        const vset_t *L = &LIVE_AT[i];
        for (uint32_t k = 0; k < L->n; k++) {
            int v = (int)L->dense[k];
            if (v != d) INTERF[d][v] = INTERF[v][d] = 1;
        }
    }
}

static void print_interference(void) {
    for (int u = 0; u < N_VARS; u++) {
        for (int v = u + 1; v < N_VARS; v++) {
            if (INTERF[u][v]) printf("  %s -- %s\n", VAR[u], VAR[v]);
        }
    }
}

/* ============================================================================
 * Register allocation as a CSP
 *
 *   Variables: one CSP variable per program variable.
 *   Domain:    [0, K-1] (K physical registers).
 *   Constraint: csp_neq for every interference edge.
 *
 * Find any feasible colouring; the first solution is optimal w.r.t.
 * register count once we've picked K. Walking K = 1, 2, 3, ... gives
 * the chromatic number of the interference graph -- that is, the
 * minimum number of registers this program needs.
 * ============================================================================ */

typedef struct { int color[N_VARS]; int found; } alloc_t;

static int color_cb(const csp_t *csp, void *ud) {
    alloc_t *a = ud;
    a->found = 1;
    for (int v = 0; v < N_VARS; v++)
        a->color[v] = csp_value(csp, (uint32_t)v);
    return 0;       /* stop after the first solution */
}

static int try_allocate(int K, int color[N_VARS]) {
    csp_t *csp = csp_new();
    var_t V[N_VARS];
    for (int v = 0; v < N_VARS; v++) V[v] = csp_var(csp, 0, K - 1);
    for (int u = 0; u < N_VARS; u++)
        for (int v = u + 1; v < N_VARS; v++)
            if (INTERF[u][v]) csp_neq(csp, V[u], V[v]);

    alloc_t a = { {0}, 0 };
    csp_solve(csp, color_cb, &a);
    csp_free(csp);
    if (a.found) memcpy(color, a.color, sizeof a.color);
    return a.found;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void) {
    printf("=== program ===\n");
    print_program();

    printf("\n=== block use / def ===\n");
    compute_block_use_def();
    for (int b = 0; b < N_BLOCKS; b++) {
        printf("  %-6s use=", BLOCKS[b].name);
        vset_print(&USE[b]);
        printf("  def=");
        vset_print(&DEF[b]);
        putchar('\n');
    }

    printf("\n=== block-level liveness ===\n");
    int iters = compute_block_liveness();
    printf("  fixpoint reached in %d iteration%s\n",
           iters, iters == 1 ? "" : "s");
    for (int b = 0; b < N_BLOCKS; b++) {
        printf("  %-6s in =", BLOCKS[b].name);
        vset_print(&LIVE_IN[b]);
        printf("  out=");
        vset_print(&LIVE_OUT[b]);
        putchar('\n');
    }

    printf("\n=== per-instruction live-out ===\n");
    compute_insn_liveness();
    for (int i = 0; i < N_INSNS; i++) {
        char line[32];
        const insn_t *I = &INSNS[i];
        switch (I->op) {
        case OP_CONST: snprintf(line, sizeof line, "%-2s = %d",        VAR[I->dst], I->s1); break;
        case OP_ADD:   snprintf(line, sizeof line, "%-2s = %s + %s",   VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
        case OP_SUB:   snprintf(line, sizeof line, "%-2s = %s - %s",   VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
        case OP_MUL:   snprintf(line, sizeof line, "%-2s = %s * %s",   VAR[I->dst], VAR[I->s1], VAR[I->s2]); break;
        case OP_BR_GT: snprintf(line, sizeof line, "br_gt %s",         VAR[I->s1]); break;
        case OP_RET:   snprintf(line, sizeof line, "ret  %s",          VAR[I->s1]); break;
        }
        printf("  %2d   %-20s // ", i, line);
        vset_print(&LIVE_AT[i]);
        putchar('\n');
    }

    printf("\n=== def-use chains ===\n");
    print_def_use_chains();

    printf("\n=== interference graph ===\n");
    compute_interference();
    print_interference();

    printf("\n=== register allocation ===\n");
    int color[N_VARS];
    int K = 1, K_max = 8;
    while (K <= K_max && !try_allocate(K, color)) {
        printf("  k = %d: infeasible\n", K);
        K++;
    }
    if (K > K_max) {
        printf("  failed within %d registers (graph too dense)\n", K_max);
        return 1;
    }
    printf("  k = %d: feasible\n", K);
    static const char *REG[] = {"r0","r1","r2","r3","r4","r5","r6","r7"};
    for (int v = 0; v < N_VARS; v++)
        printf("    %s -> %s\n", VAR[v], REG[color[v]]);

    printf("\n=== rewritten program ===\n");
    for (int b = 0; b < N_BLOCKS; b++) {
        const block_t *B = &BLOCKS[b];
        printf("%s:\n", B->name);
        for (int i = B->start; i < B->end; i++) {
            const insn_t *I = &INSNS[i];
            printf("  %2d   ", i);
            switch (I->op) {
            case OP_CONST:
                printf("%s = %d", REG[color[I->dst]], I->s1); break;
            case OP_ADD:
                printf("%s = %s + %s", REG[color[I->dst]],
                       REG[color[I->s1]], REG[color[I->s2]]); break;
            case OP_SUB:
                printf("%s = %s - %s", REG[color[I->dst]],
                       REG[color[I->s1]], REG[color[I->s2]]); break;
            case OP_MUL:
                printf("%s = %s * %s", REG[color[I->dst]],
                       REG[color[I->s1]], REG[color[I->s2]]); break;
            case OP_BR_GT:
                printf("br_gt %s", REG[color[I->s1]]); break;
            case OP_RET:
                printf("ret  %s", REG[color[I->s1]]); break;
            }
            putchar('\n');
        }
        if (B->succ[0] >= 0) {
            printf("        -> %s", BLOCKS[B->succ[0]].name);
            if (B->succ[1] >= 0) printf(", %s", BLOCKS[B->succ[1]].name);
            putchar('\n');
        }
    }

    return 0;
}
