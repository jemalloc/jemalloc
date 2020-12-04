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
	psset_bin_stats_remove(psset->stats.nonfull_slabs[pind], ps);
}

static void
psset_hpdata_heap_insert(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	hpdata_age_heap_insert(&psset->pageslabs[pind], ps);
	psset_bin_stats_insert(psset->stats.nonfull_slabs[pind], ps);
}

void
psset_insert(psset_t *psset, hpdata_t *ps) {
	assert(!hpdata_empty(ps));
	hpdata_assert_consistent(ps);
	assert(!hpdata_in_psset_get(ps));
	hpdata_in_psset_set(ps, true);
	size_t longest_free_range = hpdata_longest_free_range_get(ps);

	if (longest_free_range == 0) {
		/*
		 * We don't ned to track full slabs; just pretend to for stats
		 * purposes.  See the comment at psset_bin_stats_adjust.
		 */
		psset_bin_stats_insert(psset->stats.full_slabs, ps);
		return;
	}

	pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
	    longest_free_range << LG_PAGE));

	assert(pind < PSSET_NPSIZES);
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
	psset_hpdata_heap_insert(psset, pind, ps);
}

void
psset_remove(psset_t *psset, hpdata_t *ps) {
	hpdata_assert_consistent(ps);
	assert(hpdata_in_psset_get(ps));
	hpdata_in_psset_set(ps, false);

	size_t longest_free_range = hpdata_longest_free_range_get(ps);

	if (longest_free_range == 0) {
		psset_bin_stats_remove(psset->stats.full_slabs, ps);
		return;
	}

	pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
	    longest_free_range << LG_PAGE));
	assert(pind < PSSET_NPSIZES);
	psset_hpdata_heap_remove(psset, pind, ps);
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		bitmap_set(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
}

hpdata_t *
psset_fit(psset_t *psset, size_t size) {
	pszind_t min_pind = sz_psz2ind(sz_psz_quantize_ceil(size));
	pszind_t pind = (pszind_t)bitmap_ffu(psset->bitmap, &psset_bitmap_info,
	    (size_t)min_pind);
	if (pind == PSSET_NPSIZES) {
		return NULL;
	}
	hpdata_t *ps = hpdata_age_heap_first(&psset->pageslabs[pind]);
	if (ps == NULL) {
		return NULL;
	}

	hpdata_assert_consistent(ps);

	return ps;
}
