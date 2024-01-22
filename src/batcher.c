#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/batcher.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bit_util.h"

enum {
    /*
     * We don't actually need 3 states here; an elem that's empty (because a
     * push hasn't started yet) is not really any different from one that's
     * mid-push (because a push hasn't finished yet). But tracking states a
     * little more explicitly is cheap and can help with debugging.
     */
    batcher_elem_state_full = 0,
    batcher_elem_state_empty = 1,
    batcher_elem_state_pending = 2,  // Push has started but not finished
                                     // accessing the element.
};

static uint32_t
batcher_empty_mask(int nelems) {
    uint32_t mask = (nelems == 0 ? 0 : (~(uint32_t)0 >> (32 - nelems)));
    return mask;
}

void
batcher_init(batcher_t *batcher, batcher_elem_t *elems, int nelems) {
    assert(0 <= nelems && nelems <= BATCHER_MAX_ELEMS);
    atomic_store_u32(
      &batcher->empty_elems, batcher_empty_mask(nelems), ATOMIC_RELAXED);
    batcher->nelems = (uint8_t)nelems;
    for (int i = 0; i < nelems; i++) {
        atomic_store_u8(
          &elems[i].state, batcher_elem_state_empty, ATOMIC_RELAXED);
    }
}

int
batcher_push_begin(batcher_t *batcher, batcher_elem_t *elems) {
    uint32_t empty_elems =
      atomic_load_u32(&batcher->empty_elems, ATOMIC_RELAXED);
    while (true) {
        if (empty_elems == 0) {
            return BATCHER_NO_IDX;
        }
        unsigned idx = ffs_u32(empty_elems);
        uint32_t new_empty_elems = empty_elems ^ ((uint32_t)1 << idx);
        bool success = atomic_compare_exchange_weak_u32(&batcher->empty_elems,
          &empty_elems, new_empty_elems, ATOMIC_ACQUIRE, ATOMIC_RELAXED);
        if (success) {
            assert(atomic_load_u8(&elems[idx].state, ATOMIC_RELAXED)
              == batcher_elem_state_empty);
            atomic_store_u8(
              &elems[idx].state, batcher_elem_state_pending, ATOMIC_RELAXED);
            return idx;
        }
    }
}

void
batcher_push_end(batcher_t *batcher, batcher_elem_t *elems, int idx) {
    assert((atomic_load_u32(&batcher->empty_elems, ATOMIC_RELAXED)
             & ((uint32_t)1 << idx))
      == 0);
    atomic_store_u8(&elems[idx].state, batcher_elem_state_full, ATOMIC_RELEASE);
}

bool
batcher_pop_begin(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter) {
    uint32_t empty_elems =
      atomic_load_u32(&batcher->empty_elems, ATOMIC_RELAXED);
    uint32_t empty_mask = batcher_empty_mask(batcher->nelems);
    if (empty_elems == empty_mask) {
        return false;
    }
    iter->elems_to_visit = ~empty_elems & empty_mask;
    iter->elems_to_reset = 0;
    return true;
}

int
batcher_pop_next(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter) {
    while (iter->elems_to_visit != 0) {
        unsigned idx = ffs_u32(iter->elems_to_visit);
        uint32_t idx_mask = (uint32_t)1 << idx;
        iter->elems_to_visit ^= idx_mask;
        uint8_t elem_state = atomic_load_u8(&elems[idx].state, ATOMIC_ACQUIRE);
        if (elem_state != batcher_elem_state_full) {
            // Note: This assert wouldn't be correct: there's a race with push
            // in which they've claimed a bit but not yet initialized elem_data.
            // assert(elem_state == batcher_elem_state_pending);
            continue;
        }
        iter->elems_to_reset |= idx_mask;
        return (int)idx;
    }
    return BATCHER_NO_IDX;
}

void
batcher_pop_end(
  batcher_t *batcher, batcher_elem_t *elems, batcher_pop_iter_t *iter) {
    uint32_t elems_to_reset = iter->elems_to_reset;
    uint32_t empty_slots_mask = elems_to_reset;
    while (elems_to_reset != 0) {
        unsigned idx = ffs_u32(elems_to_reset);
        uint32_t idx_mask = (uint32_t)1 << idx;
        elems_to_reset ^= idx_mask;
        uint8_t elem_state = atomic_load_u8(&elems[idx].state, ATOMIC_ACQUIRE);
        assert(elem_state == batcher_elem_state_full);
        atomic_store_u8(
          &elems[idx].state, batcher_elem_state_empty, ATOMIC_RELEASE);
    }
    if (empty_slots_mask != 0) {
        atomic_fetch_or_u32(
          &batcher->empty_elems, empty_slots_mask, ATOMIC_RELEASE);
    }
}

void
batcher_prefork(batcher_t *batcher, batcher_elem_t *elems) {
    (void)batcher;
    (void)elems;
}

void
batcher_postfork_parent(batcher_t *batcher, batcher_elem_t *elems) {
    (void)batcher;
    (void)elems;
}

void
batcher_postfork_child(batcher_t *batcher, batcher_elem_t *elems) {
    /*
     * There are two cases in which we can have push/fork races:
     * - The pusher is getting its elements from a source protected by a mutex
     *   with lower rank than the popper.  In this case, we should acquire the
     *   mutex earlier as part of forking, so no pending pushes should be
     *   possible.
     * - The source of elements being pushed is unsynchronized with respect to
     *   the fork mutexes.  In this case, there's a fundamental risk of losing
     *   elements (imagine the fork had happens just prior to a
     *   batcher_push_begin; whatever elements were about to be pushed won't
     *   make it into shared data structures).  Since this can happen
     *   regardless, we can just discard the data in any slots that were
     *   mid-push (because the caller can't tell the difference between a push
     *   call that didn't quite start and one that started but didn't finish).
     *   This can't introduce any *new* bugs because the alternative to using
     *   the batcher is acquiring the lock -- if the pusher had tried that
     *   instead, the pusher could have lost the mutex acquisition race and
     *   still not have pushed its elements (under the lock) by the time of the
     *   fork.
     *
     *   Practically, the batcher's intended use case is to speed up tcache
     *   flushing to remote arenas, which *already* has this data-loss race, so
     *   it's not worth worrying too much about.  (This isn't a "real" bug
     *   because touching the allocator after fork from a multithreaded process
     *   is already disallowed; we make it mostly work as a QoI issue.  If a
     *   user program doesn't follow fork()'s preconditions, they get off easy
     *   with the risk of a memory leak of objects into thread caches, instead
     *   of a crash).
     */

    uint32_t new_empty_elems = batcher_empty_mask(batcher->nelems);
    for (unsigned i = 0; i < batcher->nelems; i++) {
        /*
         * Practically this could probably be relaxed (a fork is
         * heavyweight, and 'hits' each thread one at a time the way an IPI
         * would).  But that's hard to reason about and might confuse race
         * detectors, and acquire is cheap anyways, especially compared to a
         * fork.
         */
        uint8_t state = atomic_load_u8(&elems[i].state, ATOMIC_ACQUIRE);
        if (state == batcher_elem_state_full) {
            /*
             * Some thread finished a push before a fork.  Keep its data
             * around.
             */
            new_empty_elems ^= ((uint32_t)1 << i);
        } else if (state == batcher_elem_state_pending) {
            /*
             * Some thread was mid-push.  Discard its partial push and
             * reset state accordingly.
             */
            atomic_store_u8(
              &elems[i].state, batcher_elem_state_empty, ATOMIC_RELAXED);
        }
    }
    atomic_store_u32(&batcher->empty_elems, new_empty_elems, ATOMIC_RELAXED);
}
