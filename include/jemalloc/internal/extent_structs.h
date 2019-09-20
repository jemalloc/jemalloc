#ifndef JEMALLOC_INTERNAL_EXTENT_STRUCTS_H
#define JEMALLOC_INTERNAL_EXTENT_STRUCTS_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/slab_data.h"

/* Quantized collection of extents, with built-in LRU queue. */
struct extents_s {
	malloc_mutex_t		mtx;

	/*
	 * Quantized per size class heaps of extents.
	 *
	 * Synchronization: mtx.
	 */
	extent_heap_t		heaps[SC_NPSIZES + 1];
	atomic_zu_t		nextents[SC_NPSIZES + 1];
	atomic_zu_t		nbytes[SC_NPSIZES + 1];

	/*
	 * Bitmap for which set bits correspond to non-empty heaps.
	 *
	 * Synchronization: mtx.
	 */
	bitmap_t		bitmap[BITMAP_GROUPS(SC_NPSIZES + 1)];

	/*
	 * LRU of all extents in heaps.
	 *
	 * Synchronization: mtx.
	 */
	extent_list_t		lru;

	/*
	 * Page sum for all extents in heaps.
	 *
	 * The synchronization here is a little tricky.  Modifications to npages
	 * must hold mtx, but reads need not (though, a reader who sees npages
	 * without holding the mutex can't assume anything about the rest of the
	 * state of the extents_t).
	 */
	atomic_zu_t		npages;

	/* All stored extents must be in the same state. */
	extent_state_t		state;

	/*
	 * If true, delay coalescing until eviction; otherwise coalesce during
	 * deallocation.
	 */
	bool			delay_coalesce;
};

/*
 * The following two structs are for experimental purposes. See
 * experimental_utilization_query_ctl and
 * experimental_utilization_batch_query_ctl in src/ctl.c.
 */

struct extent_util_stats_s {
	size_t nfree;
	size_t nregs;
	size_t size;
};

struct extent_util_stats_verbose_s {
	void *slabcur_addr;
	size_t nfree;
	size_t nregs;
	size_t size;
	size_t bin_nfree;
	size_t bin_nregs;
};

#endif /* JEMALLOC_INTERNAL_EXTENT_STRUCTS_H */
