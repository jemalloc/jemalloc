#ifndef JEMALLOC_INTERNAL_BATCHER_H
#define JEMALLOC_INTERNAL_BATCHER_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/atomic.h"

enum {
    BATCHER_MAX_ELEMS = 32,
    BATCHER_NO_IDX = -1,
};

typedef struct batcher_s batcher_t;
struct batcher_s {
    atomic_u32_t empty_elems;
    uint8_t nelems;
};

typedef struct batcher_elem_s batcher_elem_t;
struct batcher_elem_s {
    atomic_u8_t state;
};

typedef struct batcher_pop_iter_s batcher_pop_iter_t;
struct batcher_pop_iter_s {
    uint32_t elems_to_visit;
    uint32_t elems_to_reset;
};

void batcher_init(batcher_t *batcher, batcher_elem_t *elems, int nelems);

/*
 * Returns an index (into some user-owned array) to use for pushing, or
 * BATCHER_NO_DIX if no index is free.
 */
int batcher_push_begin(batcher_t *batcher, batcher_elem_t *elems);
void batcher_push_end(batcher_t *batcher, batcher_elem_t *elems, int idx);

/*
 * If there are items to pop, returns true and initializes batcher_pop_iter. In
 * this case, the caller *must* iterate through the items
 * Otherwise, returns false and does nothing (in which case, neither
 * batcher_pop_next nor batcher_pop_end should be called).
 *
 * Mixing calls to these functions is *not* thread-safe with respect to one
 * another, but *are* thread-safe with respect to concurrent pushes.
 */
bool batcher_pop_begin(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter);
/*
 * If there is another item in the snapshot to visit, returns its index. If all
 * have been visited, returns BATCHER_NO_IDX.
 *
 * (Just because batcher_pop_begin returned true, it does not guarantee that
 * batcher_pop_next will not immediately return -1; there are races in which
 * this can happen.
 *
 */
int batcher_pop_next(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter);
/*
 * After a batcher_pop_begin call returning true, and then calling
 * batcher_pop_next until it returns -1, the caller must call batcher_pop_end
 * once it's done accessing any items it has touched.
 */
void batcher_pop_end(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter);

void batcher_prefork(batcher_t *batcher, batcher_elem_t *elems);
void batcher_postfork_parent(batcher_t *batcher, batcher_elem_t *elems);
void batcher_postfork_child(batcher_t *batcher, batcher_elem_t *elems);

#endif /* JEMALLOC_INTERNAL_BATCHER_H */
