#ifndef JEMALLOC_INTERNAL_ESET_H
#define JEMALLOC_INTERNAL_ESET_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/extent.h"
#include "jemalloc/internal/mutex.h"

/*
 * An eset ("extent set") is a quantized collection of extents, with built-in
 * LRU queue.
 */
typedef struct eset_s eset_t;
struct eset_s {
	malloc_mutex_t mtx;

	/*
	 * Quantized per size class heaps of extents.
	 *
	 * Synchronization: mtx.
	 */
	extent_heap_t heaps[SC_NPSIZES + 1];
	atomic_zu_t nextents[SC_NPSIZES + 1];
	atomic_zu_t nbytes[SC_NPSIZES + 1];

	/*
	 * Bitmap for which set bits correspond to non-empty heaps.
	 *
	 * Synchronization: mtx.
	 */
	bitmap_t bitmap[BITMAP_GROUPS(SC_NPSIZES + 1)];

	/*
	 * LRU of all extents in heaps.
	 *
	 * Synchronization: mtx.
	 */
	extent_list_t lru;

	/*
	 * Page sum for all extents in heaps.
	 *
	 * The synchronization here is a little tricky.  Modifications to npages
	 * must hold mtx, but reads need not (though, a reader who sees npages
	 * without holding the mutex can't assume anything about the rest of the
	 * state of the eset_t).
	 */
	atomic_zu_t npages;

	/* All stored extents must be in the same state. */
	extent_state_t state;

	/*
	 * If true, delay coalescing until eviction; otherwise coalesce during
	 * deallocation.
	 */
	bool delay_coalesce;
};

bool eset_init(tsdn_t *tsdn, eset_t *eset, extent_state_t state,
    bool delay_coalesce);
extent_state_t eset_state_get(const eset_t *eset);

size_t eset_npages_get(eset_t *eset);
/* Get the number of extents in the given page size index. */
size_t eset_nextents_get(eset_t *eset, pszind_t ind);
/* Get the sum total bytes of the extents in the given page size index. */
size_t eset_nbytes_get(eset_t *eset, pszind_t ind);

void eset_insert_locked(tsdn_t *tsdn, eset_t *eset, extent_t *extent);
void eset_remove_locked(tsdn_t *tsdn, eset_t *eset, extent_t *extent);
/*
 * Select an extent from this eset of the given size and alignment.  Returns
 * null if no such item could be found.
 */
extent_t *eset_fit_locked(tsdn_t *tsdn, eset_t *eset, size_t esize,
    size_t alignment);

void eset_prefork(tsdn_t *tsdn, eset_t *eset);
void eset_postfork_parent(tsdn_t *tsdn, eset_t *eset);
void eset_postfork_child(tsdn_t *tsdn, eset_t *eset);

#endif /* JEMALLOC_INTERNAL_ESET_H */
