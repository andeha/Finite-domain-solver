# csp

> An embeddable CSP solver in C99. No dependencies, no surprises.

A small finite-domain constraint solver built around two ideas:

- **Briggs sparse sets** for variable domains. Removal is O(1) by swap-and-pop, membership is O(1) through the inverse index.
- **A flat undo trail.** Every reversible mutation records `(addr, old_value)` before it writes, and `trail_restore(mark)` replays everything newer than the mark in reverse. Backtracking is O(k) in the number of removals since the choice point, never proportional to domain size.

About 500 lines of solver, 400 of examples, 65 of tests. It depends on libc and nothing else. No globals, no allocator overrides, no thread-local state.

## Build

```sh
make           # build examples
make run       # build and run the five demos
make clean
```

GCC or Clang in `-std=c99 -Wall -Wextra -Wpedantic`. Clean under `-fsanitize=address,undefined`.

## A worked example

```c
#include "csp.h"
#include <stdio.h>

static int show(const csp_t *csp, void *ud) {
    var_t *v = ud;
    printf("x = %d, y = %d\n",
           csp_value(csp, v[0]), csp_value(csp, v[1]));
    return 0;                  /* stop after first solution */
}

int main(void) {
    csp_t *csp = csp_new();
    var_t  x   = csp_var(csp, 0, 9);
    var_t  y   = csp_var(csp, 0, 9);
    csp_neq(csp, x, y);
    csp_eq_c(csp, x, 3);
    var_t  v[] = { x, y };
    csp_solve(csp, show, v);
    csp_free(csp);
    return 0;
}
```

## Layout

- `trail.{h,c}` (~80 lines). The undo log. `trail_save(addr)` records the current word; `trail_mark()` snapshots the log length; `trail_restore(mark)` rolls back everything newer than the mark.
- `csp.{h,c}` (~500 lines). Variables, constraints, propagation, search. Domains are sparse sets layered on the trail.
- `examples.c` (~400 lines). Five small problems wired to the same API.
- `test.c`. Counts solutions to N-queens (n = 4..8) and complete-graph 3-colourings, asserting against the known values from the chromatic polynomial.

## Constraints

| Posted by                        | Meaning                          |
|----------------------------------|----------------------------------|
| `csp_neq(x, y)`                  | x ≠ y                            |
| `csp_eq(x, y)`                   | x = y                            |
| `csp_neq_c(x, c)`                | x ≠ c                            |
| `csp_eq_c(x, c)`                 | x = c                            |
| `csp_alldiff(vs, n)`             | pairwise ≠ (decomposed)          |
| `csp_allowed_pairs(x, y, p, n)`  | (x, y) must appear in p          |
| `csp_forbidden_pairs(x, y, p, n)`| (x, y) must not appear in p      |

Propagation is AC-3 to fixpoint. Search is depth-first with first-fail variable ordering (smallest unfixed domain first) and value ordering by the domain's current sparse-set order.

## The five examples

1. **Label placement.** Five cities, four candidate corners per label. Box-overlap geometry is precomputed and posted as `forbidden_pairs` per city pair.
2. **Scheduling.** Four jobs in a 2-machine × 3-slot grid. `alldiff` for the resource conflict, `forbidden_pairs` for precedence, unary constraints to pin specific machines.
3. **Configuration.** A four-component PC build expressed as three `allowed_pairs` compatibility tables. Fixing the GPU collapses the rest by transitive propagation.
4. **Layout.** Four rooms in a 2×2 grid with adjacency requirements: `allowed_pairs` for "must share a wall", `forbidden_pairs` for "must not".
5. **Timetabling.** Four courses, two rooms, three slots. `alldiff` for cell uniqueness; a "must run in different slots" helper for shared-teacher pairs.

## What's deliberately not here

- **No propagation queue.** The fixpoint loop re-runs every constraint until a sweep produces no change. Adding a queue keyed by recently-touched variables is roughly 30 lines.
- **No specialised `alldiff`.** It's decomposed to pairwise ≠, which is forward-checking strength, not Régin's matching-based GAC.
- **No optimisation.** No objective, no branch-and-bound, no LDS.
- **No global constraints beyond `alldiff`.** No `cumulative`, no `element`, no `circuit`.
- **No bitset domains.** Sparse-set is fine up to several thousand values per variable; at that boundary it's worth adding a bitset alternative for dense small domains.
- **No symmetry breaking, nogood recording, or restart.**

These are the obvious places to grow. The trail layer does not need to change for any of them.



