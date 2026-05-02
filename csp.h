/*
 * csp.h -- a tiny finite-domain CSP solver built on the trail core.
 *
 * Each variable's domain is a Briggs sparse set: removal is O(1) by
 * swap-and-pop, and the inverse index lets membership tests run in O(1)
 * too. Every mutation records its before-state on the trail, so
 * backtracking is O(k) in the number of removals since the choice
 * point -- never proportional to domain size.
 *
 * Constraint set is intentionally narrow:
 *   - x != y           (binary disequality)
 *   - x == y           (binary equality / channeling)
 *   - x != c, x == c   (unary)
 *   - alldiff(...)     (decomposed to pairwise !=)
 *   - allowed_pairs    (tuple whitelist for binary)
 *   - forbidden_pairs  (tuple blacklist for binary)
 *
 * Search is depth-first with first-fail variable ordering (smallest
 * unfixed domain) and value ordering by the domain's current sparse-set
 * order. Propagation is naive AC-3-flavoured fixpoint.
 */
#ifndef CSP_H
#define CSP_H

#include <stddef.h>
#include <stdint.h>

typedef struct csp csp_t;
typedef uint32_t   var_t;

/* ---- Lifecycle ------------------------------------------------------- */
csp_t   *csp_new(void);
void     csp_free(csp_t *csp);

/* ---- Variables: integer domain [min, max] inclusive ------------------ */
var_t    csp_var(csp_t *csp, int32_t min, int32_t max);
uint32_t csp_dom_size(const csp_t *csp, var_t v);
int      csp_in_dom(const csp_t *csp, var_t v, int32_t value);
/* Read the value of a fixed variable (asserts dom size == 1). */
int32_t  csp_value(const csp_t *csp, var_t v);

/* ---- Constraints ----------------------------------------------------- */
void csp_neq    (csp_t *csp, var_t x, var_t y);            /* x != y    */
void csp_eq     (csp_t *csp, var_t x, var_t y);            /* x == y    */
void csp_neq_c  (csp_t *csp, var_t x, int32_t c);          /* x != c    */
void csp_eq_c   (csp_t *csp, var_t x, int32_t c);          /* x == c    */
void csp_alldiff(csp_t *csp, const var_t *vars, uint32_t n);

/* Binary table constraints. The pairs array is copied; caller may free. */
void csp_allowed_pairs  (csp_t *csp, var_t x, var_t y,
                         const int32_t (*pairs)[2], uint32_t n);
void csp_forbidden_pairs(csp_t *csp, var_t x, var_t y,
                         const int32_t (*pairs)[2], uint32_t n);

/* ---- Search ---------------------------------------------------------- *
 * Callback receives the CSP at each solution; return 1 to keep going,
 * 0 to stop. Returns the number of solutions found. cb may be NULL,
 * in which case csp_solve counts solutions.
 */
typedef int (*csp_solution_cb)(const csp_t *csp, void *ud);
size_t   csp_solve(csp_t *csp, csp_solution_cb cb, void *ud);

#endif /* CSP_H */
