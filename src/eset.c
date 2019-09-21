#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/eset.h"
/* For opt_retain */
#include "jemalloc/internal/extent_mmap.h"

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

extent_state_t
eset_state_get(const eset_t *eset) {
	return eset->state;
}

size_t
eset_npages_get(eset_t *eset) {
	return atomic_load_zu(&eset->npages, ATOMIC_RELAXED);
}

size_t
eset_nextents_get(eset_t *eset, pszind_t pind) {
	return atomic_load_zu(&eset->nextents[pind], ATOMIC_RELAXED);
}

size_t
eset_nbytes_get(eset_t *eset, pszind_t pind) {
	return atomic_load_zu(&eset->nbytes[pind], ATOMIC_RELAXED);
}

static void
eset_stats_add(eset_t *eset, pszind_t pind, size_t sz) {
	size_t cur = atomic_load_zu(&eset->nextents[pind], ATOMIC_RELAXED);
	atomic_store_zu(&eset->nextents[pind], cur + 1, ATOMIC_RELAXED);
	cur = atomic_load_zu(&eset->nbytes[pind], ATOMIC_RELAXED);
	atomic_store_zu(&eset->nbytes[pind], cur + sz, ATOMIC_RELAXED);
}

static void
eset_stats_sub(eset_t *eset, pszind_t pind, size_t sz) {
	size_t cur = atomic_load_zu(&eset->nextents[pind], ATOMIC_RELAXED);
	atomic_store_zu(&eset->nextents[pind], cur - 1, ATOMIC_RELAXED);
	cur = atomic_load_zu(&eset->nbytes[pind], ATOMIC_RELAXED);
	atomic_store_zu(&eset->nbytes[pind], cur - sz, ATOMIC_RELAXED);
}

void
eset_insert_locked(tsdn_t *tsdn, eset_t *eset, extent_t *extent) {
	malloc_mutex_assert_owner(tsdn, &eset->mtx);
	assert(extent_state_get(extent) == eset->state);

	size_t size = extent_size_get(extent);
	size_t psz = sz_psz_quantize_floor(size);
	pszind_t pind = sz_psz2ind(psz);
	if (extent_heap_empty(&eset->heaps[pind])) {
		bitmap_unset(eset->bitmap, &eset_bitmap_info,
		    (size_t)pind);
	}
	extent_heap_insert(&eset->heaps[pind], extent);

	if (config_stats) {
		eset_stats_add(eset, pind, size);
	}

	extent_list_append(&eset->lru, extent);
	size_t npages = size >> LG_PAGE;
	/*
	 * All modifications to npages hold the mutex (as asserted above), so we
	 * don't need an atomic fetch-add; we can get by with a load followed by
	 * a store.
	 */
	size_t cur_eset_npages =
	    atomic_load_zu(&eset->npages, ATOMIC_RELAXED);
	atomic_store_zu(&eset->npages, cur_eset_npages + npages,
	    ATOMIC_RELAXED);
}

void
eset_remove_locked(tsdn_t *tsdn, eset_t *eset, extent_t *extent) {
	malloc_mutex_assert_owner(tsdn, &eset->mtx);
	assert(extent_state_get(extent) == eset->state);

	size_t size = extent_size_get(extent);
	size_t psz = sz_psz_quantize_floor(size);
	pszind_t pind = sz_psz2ind(psz);
	extent_heap_remove(&eset->heaps[pind], extent);

	if (config_stats) {
		eset_stats_sub(eset, pind, size);
	}

	if (extent_heap_empty(&eset->heaps[pind])) {
		bitmap_set(eset->bitmap, &eset_bitmap_info,
		    (size_t)pind);
	}
	extent_list_remove(&eset->lru, extent);
	size_t npages = size >> LG_PAGE;
	/*
	 * As in eset_insert_locked, we hold eset->mtx and so don't need atomic
	 * operations for updating eset->npages.
	 */
	size_t cur_extents_npages =
	    atomic_load_zu(&eset->npages, ATOMIC_RELAXED);
	assert(cur_extents_npages >= npages);
	atomic_store_zu(&eset->npages,
	    cur_extents_npages - (size >> LG_PAGE), ATOMIC_RELAXED);
}

/*
 * Find an extent with size [min_size, max_size) to satisfy the alignment
 * requirement.  For each size, try only the first extent in the heap.
 */
static extent_t *
eset_fit_alignment(eset_t *eset, size_t min_size, size_t max_size,
    size_t alignment) {
        pszind_t pind = sz_psz2ind(sz_psz_quantize_ceil(min_size));
        pszind_t pind_max = sz_psz2ind(sz_psz_quantize_ceil(max_size));

	for (pszind_t i = (pszind_t)bitmap_ffu(eset->bitmap,
	    &eset_bitmap_info, (size_t)pind); i < pind_max; i =
	    (pszind_t)bitmap_ffu(eset->bitmap, &eset_bitmap_info,
	    (size_t)i+1)) {
		assert(i < SC_NPSIZES);
		assert(!extent_heap_empty(&eset->heaps[i]));
		extent_t *extent = extent_heap_first(&eset->heaps[i]);
		uintptr_t base = (uintptr_t)extent_base_get(extent);
		size_t candidate_size = extent_size_get(extent);
		assert(candidate_size >= min_size);

		uintptr_t next_align = ALIGNMENT_CEILING((uintptr_t)base,
		    PAGE_CEILING(alignment));
		if (base > next_align || base + candidate_size <= next_align) {
			/* Overflow or not crossing the next alignment. */
			continue;
		}

		size_t leadsize = next_align - base;
		if (candidate_size - leadsize >= min_size) {
			return extent;
		}
	}

	return NULL;
}

/*
 * Do first-fit extent selection, i.e. select the oldest/lowest extent that is
 * large enough.
 */
static extent_t *
eset_first_fit_locked(tsdn_t *tsdn, eset_t *eset, size_t size) {
	extent_t *ret = NULL;

	pszind_t pind = sz_psz2ind(sz_psz_quantize_ceil(size));

	if (!maps_coalesce && !opt_retain) {
		/*
		 * No split / merge allowed (Windows w/o retain). Try exact fit
		 * only.
		 */
		return extent_heap_empty(&eset->heaps[pind]) ? NULL :
		    extent_heap_first(&eset->heaps[pind]);
	}

	for (pszind_t i = (pszind_t)bitmap_ffu(eset->bitmap,
	    &eset_bitmap_info, (size_t)pind);
	    i < SC_NPSIZES + 1;
	    i = (pszind_t)bitmap_ffu(eset->bitmap, &eset_bitmap_info,
	    (size_t)i+1)) {
		assert(!extent_heap_empty(&eset->heaps[i]));
		extent_t *extent = extent_heap_first(&eset->heaps[i]);
		assert(extent_size_get(extent) >= size);
		/*
		 * In order to reduce fragmentation, avoid reusing and splitting
		 * large eset for much smaller sizes.
		 *
		 * Only do check for dirty eset (delay_coalesce).
		 */
		if (eset->delay_coalesce &&
		    (sz_pind2sz(i) >> opt_lg_extent_max_active_fit) > size) {
			break;
		}
		if (ret == NULL || extent_snad_comp(extent, ret) < 0) {
			ret = extent;
		}
		if (i == SC_NPSIZES) {
			break;
		}
		assert(i < SC_NPSIZES);
	}

	return ret;
}

extent_t *
eset_fit_locked(tsdn_t *tsdn, eset_t *eset, size_t esize, size_t alignment) {
	malloc_mutex_assert_owner(tsdn, &eset->mtx);

	size_t max_size = esize + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (max_size < esize) {
		return NULL;
	}

	extent_t *extent = eset_first_fit_locked(tsdn, eset, max_size);

	if (alignment > PAGE && extent == NULL) {
		/*
		 * max_size guarantees the alignment requirement but is rather
		 * pessimistic.  Next we try to satisfy the aligned allocation
		 * with sizes in [esize, max_size).
		 */
		extent = eset_fit_alignment(eset, esize, max_size, alignment);
	}

	return extent;
}

void
eset_prefork(tsdn_t *tsdn, eset_t *eset) {
	malloc_mutex_prefork(tsdn, &eset->mtx);
}

void
eset_postfork_parent(tsdn_t *tsdn, eset_t *eset) {
	malloc_mutex_postfork_parent(tsdn, &eset->mtx);
}

void
eset_postfork_child(tsdn_t *tsdn, eset_t *eset) {
	malloc_mutex_postfork_child(tsdn, &eset->mtx);
}
