#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/psset.h"

#include "jemalloc/internal/flat_bitmap.h"

static const bitmap_info_t psset_bitmap_info =
    BITMAP_INFO_INITIALIZER(PSSET_NPSIZES);

void
psset_init(psset_t *psset) {
	for (unsigned i = 0; i < PSSET_NPSIZES; i++) {
		hpdata_age_heap_new(&psset->pageslabs[i]);
	}
	bitmap_init(psset->bitmap, &psset_bitmap_info, /* fill */ true);
	memset(&psset->stats, 0, sizeof(psset->stats));
	hpdata_empty_list_init(&psset->empty_slabs);
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
 * The stats maintenance strategy is simple, but not necessarily obvious.
 * edata_nfree and the bitmap must remain consistent at all times.  If they
 * change while an edata is within an edata_heap (or full), then the associated
 * stats bin (or the full bin) must also change.  If they change while not in a
 * bin (say, in between extraction and reinsertion), then the bin stats need not
 * change.  If a pageslab is removed from a bin (or becomes nonfull), it should
 * no longer contribute to that bin's stats (or the full stats).  These help
 * ensure we don't miss any heap modification operations.
 */
JEMALLOC_ALWAYS_INLINE void
psset_bin_stats_insert_remove(psset_bin_stats_t *binstats, hpdata_t *ps,
    bool insert) {
	size_t mul = insert ? (size_t)1 : (size_t)-1;
	size_t huge_idx = (size_t)hpdata_huge_get(ps);
	binstats[huge_idx].npageslabs += mul * 1;
	binstats[huge_idx].nactive += mul * hpdata_nactive_get(ps);
	binstats[huge_idx].ndirty += mul * hpdata_ndirty_get(ps);
}

static void
psset_bin_stats_insert(psset_bin_stats_t *binstats, hpdata_t *ps) {
	psset_bin_stats_insert_remove(binstats, ps, true);
}

static void
psset_bin_stats_remove(psset_bin_stats_t *binstats, hpdata_t *ps) {
	psset_bin_stats_insert_remove(binstats, ps, false);
}

static void
psset_hpdata_heap_remove(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	hpdata_age_heap_remove(&psset->pageslabs[pind], ps);
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		bitmap_set(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
}

static void
psset_hpdata_heap_insert(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
	hpdata_age_heap_insert(&psset->pageslabs[pind], ps);
}

/*
 * Insert ps into the data structures we use to track allocation stats and pick
 * the pageslabs for new allocations.
 *
 * In particular, this does *not* remove ps from any hugification / purging
 * queues it may be in.
 */
static void
psset_do_alloc_tracking_insert(psset_t *psset, hpdata_t *ps) {
	if (hpdata_empty(ps)) {
		psset_bin_stats_insert(psset->stats.empty_slabs, ps);
		/*
		 * This prepend, paired with popping the head in psset_fit,
		 * means we implement LIFO ordering for the empty slabs set,
		 * which seems reasonable.
		 */
		hpdata_empty_list_prepend(&psset->empty_slabs, ps);
	} else if (hpdata_full(ps)) {
		psset_bin_stats_insert(psset->stats.full_slabs, ps);
		/*
		 * We don't need to keep track of the full slabs; we're never
		 * going to return them from a psset_pick_alloc call.
		 */
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_bin_stats_insert(psset->stats.nonfull_slabs[pind], ps);
		psset_hpdata_heap_insert(psset, pind, ps);
	}
}

/* Remove ps from those collections. */
static void
psset_do_alloc_tracking_remove(psset_t *psset, hpdata_t *ps) {
	if (hpdata_empty(ps)) {
		psset_bin_stats_remove(psset->stats.empty_slabs, ps);
		hpdata_empty_list_remove(&psset->empty_slabs, ps);
	} else if (hpdata_full(ps)) {
		/*
		 * We don't need to maintain an explicit container of full
		 * pageslabs anywhere, but we do have to update stats.
		 */
		psset_bin_stats_remove(psset->stats.full_slabs, ps);
	} else {
		size_t longest_free_range = hpdata_longest_free_range_get(ps);

		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);

		psset_bin_stats_remove(psset->stats.nonfull_slabs[pind], ps);
		psset_hpdata_heap_remove(psset, pind, ps);
	}
}

void
psset_update_begin(psset_t *psset, hpdata_t *ps) {
	hpdata_assert_consistent(ps);
	assert(hpdata_in_psset_get(ps));
	hpdata_updating_set(ps, true);
	psset_do_alloc_tracking_remove(psset, ps);
}

void
psset_update_end(psset_t *psset, hpdata_t *ps) {
	hpdata_assert_consistent(ps);
	assert(hpdata_in_psset_get(ps));
	hpdata_updating_set(ps, false);
	psset_do_alloc_tracking_insert(psset, ps);
}

hpdata_t *
psset_pick_alloc(psset_t *psset, size_t size) {
	assert((size & PAGE_MASK) == 0);
	assert(size <= HUGEPAGE);

	pszind_t min_pind = sz_psz2ind(sz_psz_quantize_ceil(size));
	pszind_t pind = (pszind_t)bitmap_ffu(psset->bitmap, &psset_bitmap_info,
	    (size_t)min_pind);
	if (pind == PSSET_NPSIZES) {
		return hpdata_empty_list_first(&psset->empty_slabs);
	}
	hpdata_t *ps = hpdata_age_heap_first(&psset->pageslabs[pind]);
	if (ps == NULL) {
		return NULL;
	}

	hpdata_assert_consistent(ps);

	return ps;
}

void
psset_insert(psset_t *psset, hpdata_t *ps) {
	/* We only support inserting empty pageslabs, for now. */
	assert(hpdata_empty(ps));
	hpdata_in_psset_set(ps, true);
	psset_do_alloc_tracking_insert(psset, ps);
}

void
psset_remove(psset_t *psset, hpdata_t *ps) {
	hpdata_in_psset_set(ps, false);
	psset_do_alloc_tracking_remove(psset, ps);
}
