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

JEMALLOC_ALWAYS_INLINE void
psset_assert_ps_consistent(hpdata_t *ps) {
	assert(fb_urange_longest(ps->active_pages, HUGEPAGE_PAGES)
	    == hpdata_longest_free_range_get(ps));
}

void
psset_insert(psset_t *psset, hpdata_t *ps) {
	psset_assert_ps_consistent(ps);
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
	psset_assert_ps_consistent(ps);
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
	psset_assert_ps_consistent(ps);

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

	psset_assert_ps_consistent(ps);
	return ps;
}

/*
 * Given a pageslab ps and an edata to allocate size bytes from, initializes the
 * edata with a range in the pageslab, and puts ps back in the set.
 */
static void
psset_ps_alloc_insert(psset_t *psset, hpdata_t *ps, edata_t *r_edata,
    size_t size) {
	size_t start = 0;
	/*
	 * These are dead stores, but the compiler will issue warnings on them
	 * since it can't tell statically that found is always true below.
	 */
	size_t begin = 0;
	size_t len = 0;

	fb_group_t *ps_fb = ps->active_pages;

	size_t npages = size >> LG_PAGE;

	size_t largest_unchosen_range = 0;
	while (true) {
		bool found = fb_urange_iter(ps_fb, HUGEPAGE_PAGES, start,
		    &begin, &len);
		/*
		 * A precondition to this function is that ps must be able to
		 * serve the allocation.
		 */
		assert(found);
		if (len >= npages) {
			/*
			 * We use first-fit within the page slabs; this gives
			 * bounded worst-case fragmentation within a slab.  It's
			 * not necessarily right; we could experiment with
			 * various other options.
			 */
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	uintptr_t addr = (uintptr_t)hpdata_addr_get(ps) + begin * PAGE;
	edata_init(r_edata, edata_arena_ind_get(r_edata), (void *)addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(r_edata, ps);
	fb_set_range(ps_fb, HUGEPAGE_PAGES, begin, npages);
	hpdata_nfree_set(ps, (uint32_t)(hpdata_nfree_get(ps) - npages));
	/* The pageslab isn't in a bin, so no bin stats need to change. */

	/*
	 * OK, we've got to put the pageslab back.  First we have to figure out
	 * where, though; we've only checked run sizes before the pageslab we
	 * picked.  We also need to look for ones after the one we picked.  Note
	 * that we want begin + npages as the start position, not begin + len;
	 * we might not have used the whole range.
	 *
	 * TODO: With a little bit more care, we can guarantee that the longest
	 * free range field in the edata is accurate upon entry, and avoid doing
	 * this check in the case where we're allocating from some smaller run.
	 */
	start = begin + npages;
	while (start < HUGEPAGE_PAGES) {
		bool found = fb_urange_iter(ps_fb, HUGEPAGE_PAGES, start, &begin,
		    &len);
		if (!found) {
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	hpdata_longest_free_range_set(ps, (uint32_t)largest_unchosen_range);
	if (largest_unchosen_range == 0) {
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
	fb_group_t *ps_fb = ps->active_pages;
	assert(fb_empty(ps_fb, HUGEPAGE_PAGES));
	assert(hpdata_nfree_get(ps) == HUGEPAGE_PAGES);
	psset_ps_alloc_insert(psset, ps, r_edata, size);
}

hpdata_t *
psset_dalloc(psset_t *psset, edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_ps_get(edata) != NULL);
	hpdata_t *ps = edata_ps_get(edata);

	fb_group_t *ps_fb = ps->active_pages;
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
	fb_unset_range(ps_fb, HUGEPAGE_PAGES, begin, len);

	/* The pageslab is still in the bin; adjust its stats first. */
	psset_bin_stats_t *bin_stats = (ps_old_longest_free_range == 0
	    ? &psset->stats.full_slabs : &psset->stats.nonfull_slabs[old_pind]);
	psset_bin_stats_deactivate(bin_stats, hpdata_huge_get(ps), len);

	hpdata_nfree_set(ps, (uint32_t)(hpdata_nfree_get(ps) + len));

	/* We might have just created a new, larger range. */
	size_t new_begin = (size_t)(fb_fls(ps_fb, HUGEPAGE_PAGES, begin) + 1);
	size_t new_end = fb_ffs(ps_fb, HUGEPAGE_PAGES, begin + len - 1);
	size_t new_range_len = new_end - new_begin;
	/*
	 * If the new free range is no longer than the previous longest one,
	 * then the pageslab is non-empty and doesn't need to change bins.
	 * We're done, and don't need to return a pageslab to evict.
	 */
	if (new_range_len <= ps_old_longest_free_range) {
		return NULL;
	}
	/*
	 * Otherwise, it might need to get evicted from the set, or change its
	 * bin.
	 */
	hpdata_longest_free_range_set(ps, (uint32_t)new_range_len);
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
	if (new_range_len == HUGEPAGE_PAGES) {
		return ps;
	}
	/* Otherwise, it gets reinserted. */
	pszind_t new_pind = sz_psz2ind(sz_psz_quantize_floor(
	    new_range_len << LG_PAGE));
	if (hpdata_age_heap_empty(&psset->pageslabs[new_pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info,
		    (size_t)new_pind);
	}
	psset_hpdata_heap_insert(psset, new_pind, ps);
	return NULL;
}
