#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/psset.h"

#include "jemalloc/internal/flat_bitmap.h"

void
psset_init(psset_t *psset) {
	for (unsigned i = 0; i < PSSET_NPSIZES; i++) {
		hpdata_age_heap_new(&psset->pageslabs[i]);
	}
	fb_init(psset->pageslab_bitmap, PSSET_NPSIZES);
	memset(&psset->merged_stats, 0, sizeof(psset->merged_stats));
	memset(&psset->stats, 0, sizeof(psset->stats));
	hpdata_empty_list_init(&psset->empty);
	hpdata_purge_list_init(&psset->to_purge);
	hpdata_hugify_list_init(&psset->to_hugify);
}

static void
psset_bin_stats_accum(psset_bin_stats_t *dst, psset_bin_stats_t *src) {
	dst->npageslabs += src->npageslabs;
	dst->nactive += src->nactive;
	dst->ndirty += src->ndirty;
}

void
psset_stats_accum(psset_stats_t *dst, psset_stats_t *src) {
	psset_bin_stats_accum(&dst->full_slabs[0], &src->full_slabs[0]);
	psset_bin_stats_accum(&dst->full_slabs[1], &src->full_slabs[1]);
	psset_bin_stats_accum(&dst->empty_slabs[0], &src->empty_slabs[0]);
	psset_bin_stats_accum(&dst->empty_slabs[1], &src->empty_slabs[1]);
	for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
		psset_bin_stats_accum(&dst->nonfull_slabs[i][0],
		    &src->nonfull_slabs[i][0]);
		psset_bin_stats_accum(&dst->nonfull_slabs[i][1],
		    &src->nonfull_slabs[i][1]);
	}
}

/*
 * The stats maintenance strategy is to remove a pageslab's contribution to the
 * stats when we call psset_update_begin, and re-add it (to a potentially new
 * bin) when we call psset_update_end.
 */
JEMALLOC_ALWAYS_INLINE void
psset_bin_stats_insert_remove(psset_t *psset, psset_bin_stats_t *binstats,
    hpdata_t *ps, bool insert) {
	size_t mul = insert ? (size_t)1 : (size_t)-1;
	size_t huge_idx = (size_t)hpdata_huge_get(ps);

	binstats[huge_idx].npageslabs += mul * 1;
	binstats[huge_idx].nactive += mul * hpdata_nactive_get(ps);
	binstats[huge_idx].ndirty += mul * hpdata_ndirty_get(ps);

	psset->merged_stats.npageslabs += mul * 1;
	psset->merged_stats.nactive += mul * hpdata_nactive_get(ps);
	psset->merged_stats.ndirty += mul * hpdata_ndirty_get(ps);

	if (config_debug) {
		psset_bin_stats_t check_stats = {0};
		for (size_t huge = 0; huge <= 1; huge++) {
			psset_bin_stats_accum(&check_stats,
			    &psset->stats.full_slabs[huge]);
			psset_bin_stats_accum(&check_stats,
			    &psset->stats.empty_slabs[huge]);
			for (pszind_t pind = 0; pind < PSSET_NPSIZES; pind++) {
				psset_bin_stats_accum(&check_stats,
				    &psset->stats.nonfull_slabs[pind][huge]);
			}
		}
		assert(psset->merged_stats.npageslabs
		    == check_stats.npageslabs);
		assert(psset->merged_stats.nactive == check_stats.nactive);
		assert(psset->merged_stats.ndirty == check_stats.ndirty);
	}
}

static void
psset_bin_stats_insert(psset_t *psset, psset_bin_stats_t *binstats,
    hpdata_t *ps) {
	psset_bin_stats_insert_remove(psset, binstats, ps, true);
}

static void
psset_bin_stats_remove(psset_t *psset, psset_bin_stats_t *binstats,
    hpdata_t *ps) {
	psset_bin_stats_insert_remove(psset, binstats, ps, false);
}

static void
psset_hpdata_heap_remove(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	hpdata_age_heap_remove(&psset->pageslabs[pind], ps);
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		fb_unset(psset->pageslab_bitmap, PSSET_NPSIZES, (size_t)pind);
	}
}

static void
psset_hpdata_heap_insert(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		fb_set(psset->pageslab_bitmap, PSSET_NPSIZES, (size_t)pind);
	}
	hpdata_age_heap_insert(&psset->pageslabs[pind], ps);
}

static void
psset_stats_insert(psset_t* psset, hpdata_t *ps) {
	if (hpdata_empty(ps)) {
		psset_bin_stats_insert(psset, psset->stats.empty_slabs, ps);
	} else if (hpdata_full(ps)) {
		psset_bin_stats_insert(psset, psset->stats.full_slabs, ps);
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_bin_stats_insert(psset, psset->stats.nonfull_slabs[pind],
		    ps);
	}
}

static void
psset_stats_remove(psset_t *psset, hpdata_t *ps) {
	if (hpdata_empty(ps)) {
		psset_bin_stats_remove(psset, psset->stats.empty_slabs, ps);
	} else if (hpdata_full(ps)) {
		psset_bin_stats_remove(psset, psset->stats.full_slabs, ps);
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_bin_stats_remove(psset, psset->stats.nonfull_slabs[pind],
		    ps);
	}
}

/*
 * Put ps into some container so that it can be found during future allocation
 * requests.
 */
static void
psset_alloc_container_insert(psset_t *psset, hpdata_t *ps) {
	assert(!hpdata_in_psset_alloc_container_get(ps));
	hpdata_in_psset_alloc_container_set(ps, true);
	if (hpdata_empty(ps)) {
		/*
		 * This prepend, paired with popping the head in psset_fit,
		 * means we implement LIFO ordering for the empty slabs set,
		 * which seems reasonable.
		 */
		hpdata_empty_list_prepend(&psset->empty, ps);
	} else if (hpdata_full(ps)) {
		/*
		 * We don't need to keep track of the full slabs; we're never
		 * going to return them from a psset_pick_alloc call.
		 */
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_hpdata_heap_insert(psset, pind, ps);
	}
}

/* Remove ps from those collections. */
static void
psset_alloc_container_remove(psset_t *psset, hpdata_t *ps) {
	assert(hpdata_in_psset_alloc_container_get(ps));
	hpdata_in_psset_alloc_container_set(ps, false);

	if (hpdata_empty(ps)) {
		hpdata_empty_list_remove(&psset->empty, ps);
	} else if (hpdata_full(ps)) {
		/* Same as above -- do nothing in this case. */
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_hpdata_heap_remove(psset, pind, ps);
	}
}

void
psset_update_begin(psset_t *psset, hpdata_t *ps) {
	hpdata_assert_consistent(ps);
	assert(hpdata_in_psset_get(ps));
	hpdata_updating_set(ps, true);
	psset_stats_remove(psset, ps);
	if (hpdata_in_psset_alloc_container_get(ps)) {
		/*
		 * Some metadata updates can break alloc container invariants
		 * (e.g. the longest free range determines the hpdata_heap_t the
		 * pageslab lives in).
		 */
		assert(hpdata_alloc_allowed_get(ps));
		psset_alloc_container_remove(psset, ps);
	}
	/*
	 * We don't update presence in the purge list or hugify list; we try to
	 * keep those FIFO, even in the presence of other metadata updates.
	 * We'll update presence at the end of the metadata update if necessary.
	 */
}

void
psset_update_end(psset_t *psset, hpdata_t *ps) {
	assert(hpdata_in_psset_get(ps));
	hpdata_updating_set(ps, false);
	psset_stats_insert(psset, ps);

	/*
	 * The update begin should have removed ps from whatever alloc container
	 * it was in.
	 */
	assert(!hpdata_in_psset_alloc_container_get(ps));
	if (hpdata_alloc_allowed_get(ps)) {
		psset_alloc_container_insert(psset, ps);
	}

	if (hpdata_purge_allowed_get(ps)
	    && !hpdata_in_psset_purge_container_get(ps)) {
		hpdata_in_psset_purge_container_set(ps, true);
		hpdata_purge_list_append(&psset->to_purge, ps);
	} else if (!hpdata_purge_allowed_get(ps)
	    && hpdata_in_psset_purge_container_get(ps)) {
		hpdata_in_psset_purge_container_set(ps, false);
		hpdata_purge_list_remove(&psset->to_purge, ps);
	}

	if (hpdata_hugify_allowed_get(ps)
	    && !hpdata_in_psset_hugify_container_get(ps)) {
		hpdata_in_psset_hugify_container_set(ps, true);
		hpdata_hugify_list_append(&psset->to_hugify, ps);
	} else if (!hpdata_hugify_allowed_get(ps)
	    && hpdata_in_psset_hugify_container_get(ps)) {
		hpdata_in_psset_hugify_container_set(ps, false);
		hpdata_hugify_list_remove(&psset->to_hugify, ps);
	}
	hpdata_assert_consistent(ps);
}

hpdata_t *
psset_pick_alloc(psset_t *psset, size_t size) {
	assert((size & PAGE_MASK) == 0);
	assert(size <= HUGEPAGE);

	pszind_t min_pind = sz_psz2ind(sz_psz_quantize_ceil(size));
	pszind_t pind = (pszind_t)fb_ffs(psset->pageslab_bitmap, PSSET_NPSIZES,
	    (size_t)min_pind);
	if (pind == PSSET_NPSIZES) {
		return hpdata_empty_list_first(&psset->empty);
	}
	hpdata_t *ps = hpdata_age_heap_first(&psset->pageslabs[pind]);
	if (ps == NULL) {
		return NULL;
	}

	hpdata_assert_consistent(ps);

	return ps;
}

hpdata_t *
psset_pick_purge(psset_t *psset) {
	return hpdata_purge_list_first(&psset->to_purge);
}

hpdata_t *
psset_pick_hugify(psset_t *psset) {
	return hpdata_hugify_list_first(&psset->to_hugify);
}

void
psset_insert(psset_t *psset, hpdata_t *ps) {
	hpdata_in_psset_set(ps, true);

	psset_stats_insert(psset, ps);
	if (hpdata_alloc_allowed_get(ps)) {
		psset_alloc_container_insert(psset, ps);
	}
	if (hpdata_purge_allowed_get(ps)) {
		hpdata_in_psset_purge_container_set(ps, true);
		hpdata_purge_list_append(&psset->to_purge, ps);
	}
	if (hpdata_hugify_allowed_get(ps)) {
		hpdata_in_psset_hugify_container_set(ps, true);
		hpdata_hugify_list_append(&psset->to_hugify, ps);
	}
}

void
psset_remove(psset_t *psset, hpdata_t *ps) {
	hpdata_in_psset_set(ps, false);

	psset_stats_remove(psset, ps);
	if (hpdata_in_psset_alloc_container_get(ps)) {
		psset_alloc_container_remove(psset, ps);
	}
	if (hpdata_in_psset_purge_container_get(ps)) {
		hpdata_in_psset_purge_container_set(ps, false);
		hpdata_purge_list_remove(&psset->to_purge, ps);
	}
	if (hpdata_in_psset_purge_container_get(ps)) {
		hpdata_in_psset_purge_container_set(ps, false);
		hpdata_purge_list_remove(&psset->to_purge, ps);
	}
}
