/*
 * trail.h -- a deltalog of uint32_t mutations with checkpoint/restore.
 *
 * Every mutation that should be reversible records (address, old_value)
 * before it writes. trail_mark() snapshots the current log length;
 * trail_restore(mark) pops entries back down to the mark, replaying old
 * values in reverse. Reversal is O(k) in the number of changes since the
 * mark, independent of how big the underlying state is.
 *
 * The log is a flat array; checkpoints are just sizes. There is no
 * per-checkpoint allocation, no tree, no diff. This is the cheapest
 * undo discipline the structure permits.
 */
#ifndef CSP_TRAIL_H
#define CSP_TRAIL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t *addr;     /* word that was changed                          */
    uint32_t  old;      /* value before the change                        */
} trail_entry_t;

typedef struct {
    trail_entry_t *log;
    size_t         len;
    size_t         cap;
} trail_t;

void   trail_init(trail_t *t);
void   trail_free(trail_t *t);

/* Record *addr as it stands now, so a later restore can put it back. */
void   trail_save(trail_t *t, uint32_t *addr);

/* Snapshot the log length; pass to trail_restore to roll back to here. */
size_t trail_mark(const trail_t *t);

/* Replay every saved value newer than `mark`, in reverse order. */
void   trail_restore(trail_t *t, size_t mark);

#endif /* CSP_TRAIL_H */
