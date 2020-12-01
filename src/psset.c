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
	dst->npageslabs_huge += src->npageslabs_huge;
	dst->nactive_huge += src->nactive_huge;
	dst->ninactive_huge += src->ninactive_huge;

	dst->npageslabs_nonhuge += src->npageslabs_nonhuge;
	dst->nactive_nonhuge += src->nactive_nonhuge;
	dst->ninactive_nonhuge += src->ninactive_nonhuge;
}

void
psset_stats_accum(psset_stats_t *dst, psset_stats_t *src) {
	psset_bin_stats_accum(&dst->full_slabs, &src->full_slabs);
	for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
		psset_bin_stats_accum(&dst->nonfull_slabs[i],
		    &src->nonfull_slabs[i]);
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
	size_t *npageslabs_dst = hpdata_huge_get(ps)
	    ? &binstats->npageslabs_huge : &binstats->npageslabs_nonhuge;
	size_t *nactive_dst = hpdata_huge_get(ps)
	    ? &binstats->nactive_huge : &binstats->nactive_nonhuge;
	size_t *ninactive_dst = hpdata_huge_get(ps)
	    ? &binstats->ninactive_huge : &binstats->ninactive_nonhuge;

	size_t ninactive = hpdata_nfree_get(ps);
	size_t nactive = HUGEPAGE_PAGES - ninactive;

	size_t mul = insert ? (size_t)1 : (size_t)-1;
	*npageslabs_dst += mul * 1;
	*nactive_dst += mul * nactive;
	*ninactive_dst += mul * ninactive;
}

static void
psset_bin_stats_insert(psset_bin_stats_t *binstats, hpdata_t *ps) {
	psset_bin_stats_insert_remove(binstats, ps, /* insert */ true);
}

static void
psset_bin_stats_remove(psset_bin_stats_t *binstats, hpdata_t *ps) {
	psset_bin_stats_insert_remove(binstats, ps, /* insert */ false);
}

static void
psset_hpdata_heap_remove(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	hpdata_age_heap_remove(&psset->pageslabs[pind], ps);
	psset_bin_stats_remove(&psset->stats.nonfull_slabs[pind], ps);
}

static void
psset_hpdata_heap_insert(psset_t *psset, pszind_t pind, hpdata_t *ps) {
	hpdata_age_heap_insert(&psset->pageslabs[pind], ps);
	psset_bin_stats_insert(&psset->stats.nonfull_slabs[pind], ps);
}

void
psset_insert(psset_t *psset, hpdata_t *ps) {
	assert(!hpdata_empty(ps));
	hpdata_assert_consistent(ps);
	size_t longest_free_range = hpdata_longest_free_range_get(ps);

	if (longest_free_range == 0) {
		/*
		 * We don't ned to track full slabs; just pretend to for stats
		 * purposes.  See the comment at psset_bin_stats_adjust.
		 */
		psset_bin_stats_insert(&psset->stats.full_slabs, ps);
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
	size_t longest_free_range = hpdata_longest_free_range_get(ps);

	if (longest_free_range == 0) {
		psset_bin_stats_remove(&psset->stats.full_slabs, ps);
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
