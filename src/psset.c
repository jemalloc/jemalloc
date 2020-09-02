#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/psset.h"

#include "jemalloc/internal/flat_bitmap.h"

static const bitmap_info_t psset_bitmap_info =
    BITMAP_INFO_INITIALIZER(PSSET_NPSIZES);

void
psset_init(psset_t *psset) {
	for (unsigned i = 0; i < PSSET_NPSIZES; i++) {
		edata_heap_new(&psset->pageslabs[i]);
	}
	bitmap_init(psset->bitmap, &psset_bitmap_info, /* fill */ true);
	psset->full_slab_stats.npageslabs = 0;
	psset->full_slab_stats.nactive = 0;
	psset->full_slab_stats.ninactive = 0;
	for (unsigned i = 0; i < PSSET_NPSIZES; i++) {
		psset->slab_stats[i].npageslabs = 0;
		psset->slab_stats[i].nactive = 0;
		psset->slab_stats[i].ninactive = 0;
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
psset_bin_stats_adjust(psset_bin_stats_t *binstats, edata_t *ps, bool inc) {
	size_t mul = inc ? (size_t)1 : (size_t)-1;

	size_t npages = edata_size_get(ps) >> LG_PAGE;
	size_t ninactive = edata_nfree_get(ps);
	size_t nactive = npages - ninactive;
	binstats->npageslabs += mul * 1;
	binstats->nactive += mul * nactive;
	binstats->ninactive += mul * ninactive;
}

static void
psset_edata_heap_remove(psset_t *psset, pszind_t pind, edata_t *ps) {
	edata_heap_remove(&psset->pageslabs[pind], ps);
	psset_bin_stats_adjust(&psset->slab_stats[pind], ps, /* inc */ false);
}

static void
psset_edata_heap_insert(psset_t *psset, pszind_t pind, edata_t *ps) {
	edata_heap_insert(&psset->pageslabs[pind], ps);
	psset_bin_stats_adjust(&psset->slab_stats[pind], ps, /* inc */ true);
}

JEMALLOC_ALWAYS_INLINE void
psset_assert_ps_consistent(edata_t *ps) {
	assert(fb_urange_longest(edata_slab_data_get(ps)->bitmap,
	    edata_size_get(ps) >> LG_PAGE) == edata_longest_free_range_get(ps));
}

/*
 * Similar to PAC's extent_recycle_extract.  Out of all the pageslabs in the
 * set, picks one that can satisfy the allocation and remove it from the set.
 */
static edata_t *
psset_recycle_extract(psset_t *psset, size_t size) {
	pszind_t ret_ind;
	edata_t *ret = NULL;
	pszind_t pind = sz_psz2ind(sz_psz_quantize_ceil(size));
	for (pszind_t i = (pszind_t)bitmap_ffu(psset->bitmap,
	    &psset_bitmap_info, (size_t)pind);
	    i < PSSET_NPSIZES;
	    i = (pszind_t)bitmap_ffu(psset->bitmap, &psset_bitmap_info,
		(size_t)i + 1)) {
		assert(!edata_heap_empty(&psset->pageslabs[i]));
		edata_t *ps = edata_heap_first(&psset->pageslabs[i]);
		if (ret == NULL || edata_snad_comp(ps, ret) < 0) {
			ret = ps;
			ret_ind = i;
		}
	}
	if (ret == NULL) {
		return NULL;
	}

	psset_edata_heap_remove(psset, ret_ind, ret);
	if (edata_heap_empty(&psset->pageslabs[ret_ind])) {
		bitmap_set(psset->bitmap, &psset_bitmap_info, ret_ind);
	}

	psset_assert_ps_consistent(ret);
	return ret;
}

static void
psset_insert(psset_t *psset, edata_t *ps, size_t largest_range) {
	psset_assert_ps_consistent(ps);

	pszind_t pind = sz_psz2ind(sz_psz_quantize_floor(
	    largest_range << LG_PAGE));

	assert(pind < PSSET_NPSIZES);

	if (edata_heap_empty(&psset->pageslabs[pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info, (size_t)pind);
	}
	psset_edata_heap_insert(psset, pind, ps);
}

/*
 * Given a pageslab ps and an edata to allocate size bytes from, initializes the
 * edata with a range in the pageslab, and puts ps back in the set.
 */
static void
psset_ps_alloc_insert(psset_t *psset, edata_t *ps, edata_t *r_edata,
    size_t size) {
	size_t start = 0;
	/*
	 * These are dead stores, but the compiler will issue warnings on them
	 * since it can't tell statically that found is always true below.
	 */
	size_t begin = 0;
	size_t len = 0;

	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;

	size_t npages = size >> LG_PAGE;
	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;

	size_t largest_unchosen_range = 0;
	while (true) {
		bool found = fb_urange_iter(ps_fb, ps_npages, start, &begin,
		    &len);
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
	uintptr_t addr = (uintptr_t)edata_base_get(ps) + begin * PAGE;
	edata_init(r_edata, edata_arena_ind_get(r_edata), (void *)addr, size,
	    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_active,
	    /* zeroed */ false, /* committed */ true, EXTENT_PAI_HPA,
	    EXTENT_NOT_HEAD);
	edata_ps_set(r_edata, ps);
	fb_set_range(ps_fb, ps_npages, begin, npages);
	edata_nfree_set(ps, (uint32_t)(edata_nfree_get(ps) - npages));
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
	while (start < ps_npages) {
		bool found = fb_urange_iter(ps_fb, ps_npages, start, &begin,
		    &len);
		if (!found) {
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	edata_longest_free_range_set(ps, (uint32_t)largest_unchosen_range);
	if (largest_unchosen_range == 0) {
		psset_bin_stats_adjust(&psset->full_slab_stats, ps,
		    /* inc */ true);
	} else {
		psset_insert(psset, ps, largest_unchosen_range);
	}
}

bool
psset_alloc_reuse(psset_t *psset, edata_t *r_edata, size_t size) {
	edata_t *ps = psset_recycle_extract(psset, size);
	if (ps == NULL) {
		return true;
	}
	psset_ps_alloc_insert(psset, ps, r_edata, size);
	return false;
}

void
psset_alloc_new(psset_t *psset, edata_t *ps, edata_t *r_edata, size_t size) {
	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;
	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;
	assert(fb_empty(ps_fb, ps_npages));
	assert(ps_npages >= (size >> LG_PAGE));
	edata_nfree_set(ps, (uint32_t)ps_npages);
	psset_ps_alloc_insert(psset, ps, r_edata, size);
}

edata_t *
psset_dalloc(psset_t *psset, edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_ps_get(edata) != NULL);

	edata_t *ps = edata_ps_get(edata);
	fb_group_t *ps_fb = edata_slab_data_get(ps)->bitmap;
	size_t ps_old_longest_free_range = edata_longest_free_range_get(ps);
	pszind_t old_pind = SC_NPSIZES;
	if (ps_old_longest_free_range != 0) {
		old_pind = sz_psz2ind(sz_psz_quantize_floor(
		    ps_old_longest_free_range << LG_PAGE));
	}

	size_t ps_npages = edata_size_get(ps) >> LG_PAGE;
	size_t begin =
	    ((uintptr_t)edata_base_get(edata) - (uintptr_t)edata_base_get(ps))
	    >> LG_PAGE;
	size_t len = edata_size_get(edata) >> LG_PAGE;
	fb_unset_range(ps_fb, ps_npages, begin, len);
	if (ps_old_longest_free_range == 0) {
		/* We were in the (imaginary) full bin; update stats for it. */
		psset_bin_stats_adjust(&psset->full_slab_stats, ps,
		    /* inc */ false);
	} else {
		/*
		 * The edata is still in the bin, need to update its
		 * contribution.
		 */
		psset->slab_stats[old_pind].nactive -= len;
		psset->slab_stats[old_pind].ninactive += len;
	}
	/*
	 * Note that we want to do this after the stats updates, since if it was
	 * full it psset_bin_stats_adjust would have looked at the old version.
	 */
	edata_nfree_set(ps, (uint32_t)(edata_nfree_get(ps) + len));

	/* We might have just created a new, larger range. */
	size_t new_begin = (size_t)(fb_fls(ps_fb, ps_npages, begin) + 1);
	size_t new_end = fb_ffs(ps_fb, ps_npages, begin + len - 1);
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
	edata_longest_free_range_set(ps, (uint32_t)new_range_len);
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
		psset_edata_heap_remove(psset, old_pind, ps);
		if (edata_heap_empty(&psset->pageslabs[old_pind])) {
			bitmap_set(psset->bitmap, &psset_bitmap_info,
			    (size_t)old_pind);
		}
	}
	/* If the pageslab is empty, it gets evicted from the set. */
	if (new_range_len == ps_npages) {
		return ps;
	}
	/* Otherwise, it gets reinserted. */
	pszind_t new_pind = sz_psz2ind(sz_psz_quantize_floor(
	    new_range_len << LG_PAGE));
	if (edata_heap_empty(&psset->pageslabs[new_pind])) {
		bitmap_unset(psset->bitmap, &psset_bitmap_info,
		    (size_t)new_pind);
	}
	psset_edata_heap_insert(psset, new_pind, ps);
	return NULL;
}
