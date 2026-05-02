/* Counts solutions to known problems to stress backtracking. */
#include "csp.h"
#include <stdio.h>
#include <assert.h>

/* N-queens solution counts: n=4 -> 2, n=5 -> 10, n=6 -> 4, n=7 -> 40, n=8 -> 92. */
static size_t nqueens(int n) {
    csp_t *csp = csp_new();
    var_t q[16];
    for (int i = 0; i < n; i++) q[i] = csp_var(csp, 0, n - 1);
    /* No two queens in the same column. */
    csp_alldiff(csp, q, n);
    /* No two queens on the same diagonal: q[i] - q[j] != i - j and != j - i. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int32_t bad[64][2];
            uint32_t k = 0;
            for (int a = 0; a < n; a++)
                for (int b = 0; b < n; b++)
                    if (a - b == i - j || a - b == j - i) {
                        bad[k][0] = a; bad[k][1] = b; k++;
                    }
            csp_forbidden_pairs(csp, q[i], q[j], (const int32_t (*)[2])bad, k);
        }
    }
    size_t n_sols = csp_solve(csp, NULL, NULL);
    csp_free(csp);
    return n_sols;
}

/* K_n graph 3-colourings (n vertices, complete graph): 3! * stuff for small n.
 * K3: 3*2*1 = 6. K4: 0 (chromatic number 4). */
static size_t kn_3col(int n) {
    csp_t *csp = csp_new();
    var_t v[16];
    for (int i = 0; i < n; i++) v[i] = csp_var(csp, 0, 2);
    csp_alldiff(csp, v, n);  /* complete graph: every pair differs */
    size_t r = csp_solve(csp, NULL, NULL);
    csp_free(csp);
    return r;
}

int main(void) {
    struct { int n; size_t expected; } qcases[] = {
        {4, 2}, {5, 10}, {6, 4}, {7, 40}, {8, 92},
    };
    for (size_t i = 0; i < sizeof qcases / sizeof qcases[0]; i++) {
        size_t got = nqueens(qcases[i].n);
        printf("  %d-queens: %zu (expected %zu) %s\n",
               qcases[i].n, got, qcases[i].expected,
               got == qcases[i].expected ? "OK" : "FAIL");
        assert(got == qcases[i].expected);
    }
    struct { int n; size_t expected; } kcases[] = {
        {2, 6}, {3, 6}, {4, 0},
    };
    for (size_t i = 0; i < sizeof kcases / sizeof kcases[0]; i++) {
        size_t got = kn_3col(kcases[i].n);
        printf("  K%d 3-colourings: %zu (expected %zu) %s\n",
               kcases[i].n, got, kcases[i].expected,
               got == kcases[i].expected ? "OK" : "FAIL");
        assert(got == kcases[i].expected);
    }
    return 0;
}
