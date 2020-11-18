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

/*
 * We don't currently need an "activate" equivalent to this, since down the
 * allocation pathways we don't do the optimization in which we change a slab
 * without first removing it from a bin.
 */
static void
psset_bin_stats_deactivate(psset_bin_stats_t *binstats, bool huge, size_t num) {
	size_t *nactive_dst = huge
	    ? &binstats->nactive_huge : &binstats->nactive_nonhuge;
	size_t *ninactive_dst = huge
	    ? &binstats->ninactive_huge : &binstats->ninactive_nonhuge;

	assert(*nactive_dst >= num);
	*nactive_dst -= num;
	*ninactive_dst += num;
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

void
psset_hugify(psset_t *psset, hpdata_t *ps) {
	assert(!hpdata_huge_get(ps));
	hpdata_assert_consistent(ps);

	size_t longest_free_range = hpdata_longest_free_range_get(ps);
	psset_bin_stats_t *bin_stats;
	if (longest_free_range == 0) {
		bin_stats = &psset->stats.full_slabs;
	} else {
		pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
		    longest_free_range << LG_PAGE));
		assert(pind < PSSET_NPSIZES);
		bin_stats = &psset->stats.nonfull_slabs[pind];
	}
	psset_bin_stats_remove(bin_stats, ps);
	hpdata_huge_set(ps, true);
	psset_bin_stats_insert(bin_stats, ps);
}

/*
 * Similar to PAC's extent_recycle_extract.  Out of all the pageslabs in the
 * set, picks one that can satisfy the allocation and remove it from the set.
 */
static hpdata_t *
psset_recycle_extract(psset_t *psset, size_t size) {
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

	psset_hpdata_heap_remove(psset, pind, ps);
	if (hpdata_age_heap_empty(&psset->pageslabs[pind])) {
		bitmap_set(psset->bitmap, &psset_bitmap_info, pind);
	}

	hpdata_assert_consistent(ps);
	return ps;
}

/*
 * Given a pageslab ps and an edata to allocate size bytes from, initializes the
 * edata with a range in the pageslab, and puts ps back in the set.
 */
static void
psset_ps_alloc_insert(psset_t *psset, hpdata_t *ps, edata_t *r_edata,
    size_t size) {
	size_t npages = size / PAGE;
	size_t begin = hpdata_reserve_alloc(ps, npages);
	uintptr_t addr = (uintptr_t)hpdata_addr_get(ps) + begin * PAGE;
	edata_init(r_edata, edata_arena_ind_get(r_edata), (void *)addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(r_edata, ps);
	/* The pageslab isn't in a bin, so no bin stats need to change. */

	size_t longest_free_range = hpdata_longest_free_range_get(ps);
	if (longest_free_range == 0) {
		psset_bin_stats_insert(&psset->stats.full_slabs, ps);
	} else {
		psset_insert(psset, ps);
	}
}

bool
psset_alloc_reuse(psset_t *psset, edata_t *r_edata, size_t size) {
	hpdata_t *ps = psset_recycle_extract(psset, size);
	if (ps == NULL) {
		return true;
	}
	psset_ps_alloc_insert(psset, ps, r_edata, size);
	return false;
}

void
psset_alloc_new(psset_t *psset, hpdata_t *ps, edata_t *r_edata, size_t size) {
	hpdata_assert_empty(ps);
	psset_ps_alloc_insert(psset, ps, r_edata, size);
}

hpdata_t *
psset_dalloc(psset_t *psset, edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_ps_get(edata) != NULL);
	hpdata_t *ps = edata_ps_get(edata);

	size_t ps_old_longest_free_range = hpdata_longest_free_range_get(ps);
	pszind_t old_pind = SC_NPSIZES;
	if (ps_old_longest_free_range != 0) {
		old_pind = sz_psz2ind(sz_psz_quantize_floor(
		    ps_old_longest_free_range << LG_PAGE));
	}

	size_t begin =
	    ((uintptr_t)edata_base_get(edata) - (uintptr_t)hpdata_addr_get(ps))
	    >> LG_PAGE;
	size_t len = edata_size_get(edata) >> LG_PAGE;

	/* The pageslab is still in the bin; adjust its stats first. */
	psset_bin_stats_t *bin_stats = (ps_old_longest_free_range == 0
	    ? &psset->stats.full_slabs : &psset->stats.nonfull_slabs[old_pind]);
	psset_bin_stats_deactivate(bin_stats, hpdata_huge_get(ps), len);

	hpdata_unreserve(ps, begin, len);
	size_t ps_new_longest_free_range = hpdata_longest_free_range_get(ps);

	/*
	 * If the new free range is no longer than the previous longest one,
	 * then the pageslab is non-empty and doesn't need to change bins.
	 * We're done, and don't need to return a pageslab to evict.
	 */
	if (ps_new_longest_free_range <= ps_old_longest_free_range) {
		return NULL;
	}
	/*
	 * If it was previously non-full, then it's in some (possibly now
	 * incorrect) bin already; remove it.
	 *
	 * TODO: We bailed out early above if we didn't expand the longest free
	 * range, which should avoid a lot of redundant remove/reinserts in the
	 * same bin.  But it doesn't eliminate all of them; it's possible that
	 * we decreased the longest free range length, but only slightly, and
	 * not enough to change our pszind.  We could check that more precisely.
	 * (Or, ideally, size class dequantization will happen at some point,
	 * and the issue becomes moot).
	 */
	if (ps_old_longest_free_range > 0) {
		psset_hpdata_heap_remove(psset, old_pind, ps);
		if (hpdata_age_heap_empty(&psset->pageslabs[old_pind])) {
			bitmap_set(psset->bitmap, &psset_bitmap_info,
			    (size_t)old_pind);
		}
	} else {
		/*
		 * Otherwise, the bin was full, and we need to adjust the full
		 * bin stats.
		 */
		psset_bin_stats_remove(&psset->stats.full_slabs, ps);
	}
	/* If the pageslab is empty, it gets evicted from the set. */
	if (ps_new_longest_free_range == HUGEPAGE_PAGES) {
		return ps;
	}
	/* Otherwise, it gets reinserted. */
	pszind_t new_pind = sz_psz2ind(sz_psz_quantize_floor(
	    ps_new_longest_free_range << LG_PAGE));
	if (hpdata_age_heap_empty(&psset->pageslabs[new_pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info,
		    (size_t)new_pind);
	}
	psset_hpdata_heap_insert(psset, new_pind, ps);
	return NULL;
}
