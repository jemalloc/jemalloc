#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/eset.h"

const bitmap_info_t eset_bitmap_info =
    BITMAP_INFO_INITIALIZER(SC_NPSIZES+1);

bool
eset_init(tsdn_t *tsdn, eset_t *eset, extent_state_t state,
    bool delay_coalesce) {
	if (malloc_mutex_init(&eset->mtx, "extents", WITNESS_RANK_EXTENTS,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	for (unsigned i = 0; i < SC_NPSIZES + 1; i++) {
		extent_heap_new(&eset->heaps[i]);
	}
	bitmap_init(eset->bitmap, &eset_bitmap_info, true);
	extent_list_init(&eset->lru);
	atomic_store_zu(&eset->npages, 0, ATOMIC_RELAXED);
	eset->state = state;
	eset->delay_coalesce = delay_coalesce;
	return false;
}
