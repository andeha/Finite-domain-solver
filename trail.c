#include "trail.h"

#include <stdlib.h>

void trail_init(trail_t *t) {
    t->log = NULL;
    t->len = 0;
    t->cap = 0;
}

void trail_free(trail_t *t) {
    free(t->log);
    t->log = NULL;
    t->len = t->cap = 0;
}

void trail_save(trail_t *t, uint32_t *addr) {
    if (t->len == t->cap) {
        size_t new_cap = t->cap ? t->cap * 2 : 256;
        trail_entry_t *new_log = realloc(t->log, new_cap * sizeof *new_log);
        if (!new_log) abort();          /* OOM: caller can't recover usefully */
        t->log = new_log;
        t->cap = new_cap;
    }
    t->log[t->len].addr = addr;
    t->log[t->len].old  = *addr;
    t->len++;
}

size_t trail_mark(const trail_t *t) {
    return t->len;
}

void trail_restore(trail_t *t, size_t mark) {
    while (t->len > mark) {
        t->len--;
        *t->log[t->len].addr = t->log[t->len].old;
    }
}
