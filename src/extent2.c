#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_pool.h"

/******************************************************************************/
/* Data. */

rtree_t		extents_rtree;
/* Keyed by the address of the edata_t being protected. */
mutex_pool_t	extent_mutex_pool;

size_t opt_lg_extent_max_active_fit = LG_EXTENT_MAX_ACTIVE_FIT_DEFAULT;

static bool extent_commit_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained);
static bool extent_purge_lazy_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, edata_t *edata, size_t offset, size_t length,
    bool growing_retained);
static bool extent_purge_forced_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, edata_t *edata, size_t offset, size_t length,
    bool growing_retained);
static edata_t *extent_split_impl(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, edata_t *edata, size_t size_a, szind_t szind_a,
    bool slab_a, size_t size_b, szind_t szind_b, bool slab_b,
    bool growing_retained);
static bool extent_merge_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *a, edata_t *b, bool growing_retained);

/* Used exclusively for gdump triggering. */
static atomic_zu_t curpages;
static atomic_zu_t highpages;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void extent_deregister(tsdn_t *tsdn, edata_t *edata);
static edata_t *extent_recycle(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t usize, size_t pad, size_t alignment,
    bool slab, szind_t szind, bool *zero, bool *commit, bool growing_retained);
static edata_t *extent_try_coalesce(tsdn_t *tsdn, arena_t *arena,
    ehooks_t *ehooks, rtree_ctx_t *rtree_ctx, ecache_t *ecache, edata_t *edata,
    bool *coalesced, bool growing_retained);
static void extent_record(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool growing_retained);

/******************************************************************************/

typedef enum {
	lock_result_success,
	lock_result_failure,
	lock_result_no_extent
} lock_result_t;

static inline void
extent_lock_edata(tsdn_t *tsdn, edata_t *edata) {
	assert(edata != NULL);
	mutex_pool_lock(tsdn, &extent_mutex_pool, (uintptr_t)edata);
}

static inline void
extent_unlock_edata(tsdn_t *tsdn, edata_t *edata) {
	assert(edata != NULL);
	mutex_pool_unlock(tsdn, &extent_mutex_pool, (uintptr_t)edata);
}

static inline void
extent_lock_edata2(tsdn_t *tsdn, edata_t *edata1, edata_t *edata2) {
	assert(edata1 != NULL && edata2 != NULL);
	mutex_pool_lock2(tsdn, &extent_mutex_pool, (uintptr_t)edata1,
	    (uintptr_t)edata2);
}

static inline void
extent_unlock_edata2(tsdn_t *tsdn, edata_t *edata1, edata_t *edata2) {
	assert(edata1 != NULL && edata2 != NULL);
	mutex_pool_unlock2(tsdn, &extent_mutex_pool, (uintptr_t)edata1,
	    (uintptr_t)edata2);
}

static lock_result_t
extent_rtree_leaf_elm_try_lock(tsdn_t *tsdn, rtree_leaf_elm_t *elm,
    edata_t **result, bool inactive_only) {
	edata_t *edata1 = rtree_leaf_elm_edata_read(tsdn, &extents_rtree,
	    elm, true);

	/* Slab implies active extents and should be skipped. */
	if (edata1 == NULL || (inactive_only && rtree_leaf_elm_slab_read(tsdn,
	    &extents_rtree, elm, true))) {
		return lock_result_no_extent;
	}

	/*
	 * It's possible that the extent changed out from under us, and with it
	 * the leaf->edata mapping.  We have to recheck while holding the lock.
	 */
	extent_lock_edata(tsdn, edata1);
	edata_t *edata2 = rtree_leaf_elm_edata_read(tsdn, &extents_rtree, elm,
	    true);

	if (edata1 == edata2) {
		*result = edata1;
		return lock_result_success;
	} else {
		extent_unlock_edata(tsdn, edata1);
		return lock_result_failure;
	}
}

/*
 * Returns a pool-locked edata_t * if there's one associated with the given
 * address, and NULL otherwise.
 */
static edata_t *
extent_lock_edata_from_addr(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, void *addr,
    bool inactive_only) {
	edata_t *ret = NULL;
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, &extents_rtree,
	    rtree_ctx, (uintptr_t)addr, false, false);
	if (elm == NULL) {
		return NULL;
	}
	lock_result_t lock_result;
	do {
		lock_result = extent_rtree_leaf_elm_try_lock(tsdn, elm, &ret,
		    inactive_only);
	} while (lock_result == lock_result_failure);
	return ret;
}

static void
extent_addr_randomize(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t alignment) {
	assert(edata_base_get(edata) == edata_addr_get(edata));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE -
		    lg_floor(CACHELINE_CEILING(alignment));
		size_t r;
		if (!tsdn_null(tsdn)) {
			tsd_t *tsd = tsdn_tsd(tsdn);
			r = (size_t)prng_lg_range_u64(
			    tsd_prng_statep_get(tsd), lg_range);
		} else {
			uint64_t stack_value = (uint64_t)(uintptr_t)&r;
			r = (size_t)prng_lg_range_u64(&stack_value, lg_range);
		}
		uintptr_t random_offset = ((uintptr_t)r) << (LG_PAGE -
		    lg_range);
		edata->e_addr = (void *)((uintptr_t)edata->e_addr +
		    random_offset);
		assert(ALIGNMENT_ADDR2BASE(edata->e_addr, alignment) ==
		    edata->e_addr);
	}
}

static bool
extent_try_delayed_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, edata_t *edata) {
	edata_state_set(edata, extent_state_active);
	bool coalesced;
	edata = extent_try_coalesce(tsdn, arena, ehooks, rtree_ctx, ecache,
	    edata, &coalesced, false);
	edata_state_set(edata, ecache->state);

	if (!coalesced) {
		return true;
	}
	eset_insert(&ecache->eset, edata);
	return false;
}

edata_t *
extents_alloc(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, ecache_t *ecache,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	assert(size + pad != 0);
	assert(alignment != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_t *edata = extent_recycle(tsdn, arena, ehooks, ecache, new_addr,
	    size, pad, alignment, slab, szind, zero, commit, false);
	assert(edata == NULL || edata_dumpable_get(edata));
	return edata;
}

void
extents_dalloc(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, ecache_t *ecache,
    edata_t *edata) {
	assert(edata_base_get(edata) != NULL);
	assert(edata_size_get(edata) != 0);
	assert(edata_dumpable_get(edata));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_addr_set(edata, edata_base_get(edata));
	edata_zeroed_set(edata, false);

	extent_record(tsdn, arena, ehooks, ecache, edata, false);
}

edata_t *
extents_evict(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, ecache_t *ecache,
    size_t npages_min) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	malloc_mutex_lock(tsdn, &ecache->mtx);

	/*
	 * Get the LRU coalesced extent, if any.  If coalescing was delayed,
	 * the loop will iterate until the LRU extent is fully coalesced.
	 */
	edata_t *edata;
	while (true) {
		/* Get the LRU extent, if any. */
		edata = edata_list_first(&ecache->eset.lru);
		if (edata == NULL) {
			goto label_return;
		}
		/* Check the eviction limit. */
		size_t extents_npages = ecache_npages_get(ecache);
		if (extents_npages <= npages_min) {
			edata = NULL;
			goto label_return;
		}
		eset_remove(&ecache->eset, edata);
		if (!ecache->delay_coalesce) {
			break;
		}
		/* Try to coalesce. */
		if (extent_try_delayed_coalesce(tsdn, arena, ehooks, rtree_ctx,
		    ecache, edata)) {
			break;
		}
		/*
		 * The LRU extent was just coalesced and the result placed in
		 * the LRU at its neighbor's position.  Start over.
		 */
	}

	/*
	 * Either mark the extent active or deregister it to protect against
	 * concurrent operations.
	 */
	switch (ecache->state) {
	case extent_state_active:
		not_reached();
	case extent_state_dirty:
	case extent_state_muzzy:
		edata_state_set(edata, extent_state_active);
		break;
	case extent_state_retained:
		extent_deregister(tsdn, edata);
		break;
	default:
		not_reached();
	}

label_return:
	malloc_mutex_unlock(tsdn, &ecache->mtx);
	return edata;
}

/*
 * This can only happen when we fail to allocate a new extent struct (which
 * indicates OOM), e.g. when trying to split an existing extent.
 */
static void
extents_abandon_vm(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool growing_retained) {
	size_t sz = edata_size_get(edata);
	if (config_stats) {
		arena_stats_accum_zu(&arena->stats.abandoned_vm, sz);
	}
	/*
	 * Leak extent after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (ecache->state == extent_state_dirty) {
		if (extent_purge_lazy_impl(tsdn, arena, ehooks, edata, 0, sz,
		    growing_retained)) {
			extent_purge_forced_impl(tsdn, arena, ehooks, edata, 0,
			    edata_size_get(edata), growing_retained);
		}
	}
	edata_cache_put(tsdn, &arena->edata_cache, edata);
}

static void
extent_deactivate_locked(tsdn_t *tsdn, arena_t *arena, ecache_t *ecache,
    edata_t *edata) {
	assert(edata_arena_ind_get(edata) == arena_ind_get(arena));
	assert(edata_state_get(edata) == extent_state_active);

	edata_state_set(edata, ecache->state);
	eset_insert(&ecache->eset, edata);
}

static void
extent_deactivate(tsdn_t *tsdn, arena_t *arena, ecache_t *ecache,
    edata_t *edata) {
	malloc_mutex_lock(tsdn, &ecache->mtx);
	extent_deactivate_locked(tsdn, arena, ecache, edata);
	malloc_mutex_unlock(tsdn, &ecache->mtx);
}

static void
extent_activate_locked(tsdn_t *tsdn, arena_t *arena, ecache_t *ecache,
    edata_t *edata) {
	assert(edata_arena_ind_get(edata) == arena_ind_get(arena));
	assert(edata_state_get(edata) == ecache->state);

	eset_remove(&ecache->eset, edata);
	edata_state_set(edata, extent_state_active);
}

static bool
extent_rtree_leaf_elms_lookup(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const edata_t *edata, bool dependent, bool init_missing,
    rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b) {
	*r_elm_a = rtree_leaf_elm_lookup(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL) {
		return true;
	}
	assert(*r_elm_a != NULL);

	*r_elm_b = rtree_leaf_elm_lookup(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)edata_last_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_b == NULL) {
		return true;
	}
	assert(*r_elm_b != NULL);

	return false;
}

static void
extent_rtree_write_acquired(tsdn_t *tsdn, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, edata_t *edata, szind_t szind, bool slab) {
	rtree_leaf_elm_write(tsdn, &extents_rtree, elm_a, edata, szind, slab);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, elm_b, edata, szind,
		    slab);
	}
}

static void
extent_interior_register(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, edata_t *edata,
    szind_t szind) {
	assert(edata_slab_get(edata));

	/* Register interior. */
	for (size_t i = 1; i < (edata_size_get(edata) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)edata_base_get(edata) + (uintptr_t)(i <<
		    LG_PAGE), edata, szind, true);
	}
}

static void
extent_gdump_add(tsdn_t *tsdn, const edata_t *edata) {
	cassert(config_prof);
	/* prof_gdump() requirement. */
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (opt_prof && edata_state_get(edata) == extent_state_active) {
		size_t nadd = edata_size_get(edata) >> LG_PAGE;
		size_t cur = atomic_fetch_add_zu(&curpages, nadd,
		    ATOMIC_RELAXED) + nadd;
		size_t high = atomic_load_zu(&highpages, ATOMIC_RELAXED);
		while (cur > high && !atomic_compare_exchange_weak_zu(
		    &highpages, &high, cur, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highpages update race.
			 * Note that high is updated in case of CAS failure.
			 */
		}
		if (cur > high && prof_gdump_get_unlocked()) {
			prof_gdump(tsdn);
		}
	}
}

static void
extent_gdump_sub(tsdn_t *tsdn, const edata_t *edata) {
	cassert(config_prof);

	if (opt_prof && edata_state_get(edata) == extent_state_active) {
		size_t nsub = edata_size_get(edata) >> LG_PAGE;
		assert(atomic_load_zu(&curpages, ATOMIC_RELAXED) >= nsub);
		atomic_fetch_sub_zu(&curpages, nsub, ATOMIC_RELAXED);
	}
}

static bool
extent_register_impl(tsdn_t *tsdn, edata_t *edata, bool gdump_add) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;

	/*
	 * We need to hold the lock to protect against a concurrent coalesce
	 * operation that sees us in a partial state.
	 */
	extent_lock_edata(tsdn, edata);

	if (extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, edata, false, true,
	    &elm_a, &elm_b)) {
		extent_unlock_edata(tsdn, edata);
		return true;
	}

	szind_t szind = edata_szind_get_maybe_invalid(edata);
	bool slab = edata_slab_get(edata);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, edata, szind, slab);
	if (slab) {
		extent_interior_register(tsdn, rtree_ctx, edata, szind);
	}

	extent_unlock_edata(tsdn, edata);

	if (config_prof && gdump_add) {
		extent_gdump_add(tsdn, edata);
	}

	return false;
}

static bool
extent_register(tsdn_t *tsdn, edata_t *edata) {
	return extent_register_impl(tsdn, edata, true);
}

static bool
extent_register_no_gdump_add(tsdn_t *tsdn, edata_t *edata) {
	return extent_register_impl(tsdn, edata, false);
}

static void
extent_reregister(tsdn_t *tsdn, edata_t *edata) {
	bool err = extent_register(tsdn, edata);
	assert(!err);
}

/*
 * Removes all pointers to the given extent from the global rtree indices for
 * its interior.  This is relevant for slab extents, for which we need to do
 * metadata lookups at places other than the head of the extent.  We deregister
 * on the interior, then, when an extent moves from being an active slab to an
 * inactive state.
 */
static void
extent_interior_deregister(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    edata_t *edata) {
	size_t i;

	assert(edata_slab_get(edata));

	for (i = 1; i < (edata_size_get(edata) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)edata_base_get(edata) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

/*
 * Removes all pointers to the given extent from the global rtree.
 */
static void
extent_deregister_impl(tsdn_t *tsdn, edata_t *edata, bool gdump) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *elm_a, *elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, edata, true, false,
	    &elm_a, &elm_b);

	extent_lock_edata(tsdn, edata);

	extent_rtree_write_acquired(tsdn, elm_a, elm_b, NULL, SC_NSIZES, false);
	if (edata_slab_get(edata)) {
		extent_interior_deregister(tsdn, rtree_ctx, edata);
		edata_slab_set(edata, false);
	}

	extent_unlock_edata(tsdn, edata);

	if (config_prof && gdump) {
		extent_gdump_sub(tsdn, edata);
	}
}

static void
extent_deregister(tsdn_t *tsdn, edata_t *edata) {
	extent_deregister_impl(tsdn, edata, true);
}

static void
extent_deregister_no_gdump_sub(tsdn_t *tsdn, edata_t *edata) {
	extent_deregister_impl(tsdn, edata, false);
}

/*
 * Tries to find and remove an extent from ecache that can be used for the
 * given allocation request.
 */
static edata_t *
extent_recycle_extract(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, void *new_addr, size_t size,
    size_t pad, size_t alignment, bool slab, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(alignment > 0);
	if (config_debug && new_addr != NULL) {
		/*
		 * Non-NULL new_addr has two use cases:
		 *
		 *   1) Recycle a known-extant extent, e.g. during purging.
		 *   2) Perform in-place expanding reallocation.
		 *
		 * Regardless of use case, new_addr must either refer to a
		 * non-existing extent, or to the base of an extant extent,
		 * since only active slabs support interior lookups (which of
		 * course cannot be recycled).
		 */
		assert(PAGE_ADDR2BASE(new_addr) == new_addr);
		assert(pad == 0);
		assert(alignment <= PAGE);
	}

	size_t esize = size + pad;
	malloc_mutex_lock(tsdn, &ecache->mtx);
	edata_t *edata;
	if (new_addr != NULL) {
		edata = extent_lock_edata_from_addr(tsdn, rtree_ctx, new_addr,
		    false);
		if (edata != NULL) {
			/*
			 * We might null-out edata to report an error, but we
			 * still need to unlock the associated mutex after.
			 */
			edata_t *unlock_edata = edata;
			assert(edata_base_get(edata) == new_addr);
			if (edata_arena_ind_get(edata) != arena_ind_get(arena)
			    || edata_size_get(edata) < esize
			    || edata_state_get(edata)
			    != ecache->state) {
				edata = NULL;
			}
			extent_unlock_edata(tsdn, unlock_edata);
		}
	} else {
		edata = eset_fit(&ecache->eset, esize, alignment,
		    ecache->delay_coalesce);
	}
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &ecache->mtx);
		return NULL;
	}

	extent_activate_locked(tsdn, arena, ecache, edata);
	malloc_mutex_unlock(tsdn, &ecache->mtx);

	return edata;
}

/*
 * Given an allocation request and an extent guaranteed to be able to satisfy
 * it, this splits off lead and trail extents, leaving edata pointing to an
 * extent satisfying the allocation.
 * This function doesn't put lead or trail into any ecache; it's the caller's
 * job to ensure that they can be reused.
 */
typedef enum {
	/*
	 * Split successfully.  lead, edata, and trail, are modified to extents
	 * describing the ranges before, in, and after the given allocation.
	 */
	extent_split_interior_ok,
	/*
	 * The extent can't satisfy the given allocation request.  None of the
	 * input edata_t *s are touched.
	 */
	extent_split_interior_cant_alloc,
	/*
	 * In a potentially invalid state.  Must leak (if *to_leak is non-NULL),
	 * and salvage what's still salvageable (if *to_salvage is non-NULL).
	 * None of lead, edata, or trail are valid.
	 */
	extent_split_interior_error
} extent_split_interior_result_t;

static extent_split_interior_result_t
extent_split_interior(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx,
    /* The result of splitting, in case of success. */
    edata_t **edata, edata_t **lead, edata_t **trail,
    /* The mess to clean up, in case of error. */
    edata_t **to_leak, edata_t **to_salvage,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool growing_retained) {
	size_t esize = size + pad;
	size_t leadsize = ALIGNMENT_CEILING((uintptr_t)edata_base_get(*edata),
	    PAGE_CEILING(alignment)) - (uintptr_t)edata_base_get(*edata);
	assert(new_addr == NULL || leadsize == 0);
	if (edata_size_get(*edata) < leadsize + esize) {
		return extent_split_interior_cant_alloc;
	}
	size_t trailsize = edata_size_get(*edata) - leadsize - esize;

	*lead = NULL;
	*trail = NULL;
	*to_leak = NULL;
	*to_salvage = NULL;

	/* Split the lead. */
	if (leadsize != 0) {
		*lead = *edata;
		*edata = extent_split_impl(tsdn, arena, ehooks, *lead,
		    leadsize, SC_NSIZES, false, esize + trailsize, szind, slab,
		    growing_retained);
		if (*edata == NULL) {
			*to_leak = *lead;
			*lead = NULL;
			return extent_split_interior_error;
		}
	}

	/* Split the trail. */
	if (trailsize != 0) {
		*trail = extent_split_impl(tsdn, arena, ehooks, *edata, esize,
		    szind, slab, trailsize, SC_NSIZES, false, growing_retained);
		if (*trail == NULL) {
			*to_leak = *edata;
			*to_salvage = *lead;
			*lead = NULL;
			*edata = NULL;
			return extent_split_interior_error;
		}
	}

	if (leadsize == 0 && trailsize == 0) {
		/*
		 * Splitting causes szind to be set as a side effect, but no
		 * splitting occurred.
		 */
		edata_szind_set(*edata, szind);
		if (szind != SC_NSIZES) {
			rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx,
			    (uintptr_t)edata_addr_get(*edata), szind, slab);
			if (slab && edata_size_get(*edata) > PAGE) {
				rtree_szind_slab_update(tsdn, &extents_rtree,
				    rtree_ctx,
				    (uintptr_t)edata_past_get(*edata) -
				    (uintptr_t)PAGE, szind, slab);
			}
		}
	}

	return extent_split_interior_ok;
}

/*
 * This fulfills the indicated allocation request out of the given extent (which
 * the caller should have ensured was big enough).  If there's any unused space
 * before or after the resulting allocation, that space is given its own extent
 * and put back into ecache.
 */
static edata_t *
extent_recycle_split(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, void *new_addr, size_t size,
    size_t pad, size_t alignment, bool slab, szind_t szind, edata_t *edata,
    bool growing_retained) {
	edata_t *lead;
	edata_t *trail;
	edata_t *to_leak;
	edata_t *to_salvage;

	extent_split_interior_result_t result = extent_split_interior(
	    tsdn, arena, ehooks, rtree_ctx, &edata, &lead, &trail, &to_leak,
	    &to_salvage, new_addr, size, pad, alignment, slab, szind,
	    growing_retained);

	if (!maps_coalesce && result != extent_split_interior_ok
	    && !opt_retain) {
		/*
		 * Split isn't supported (implies Windows w/o retain).  Avoid
		 * leaking the extent.
		 */
		assert(to_leak != NULL && lead == NULL && trail == NULL);
		extent_deactivate(tsdn, arena, ecache, to_leak);
		return NULL;
	}

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_deactivate(tsdn, arena, ecache, lead);
		}
		if (trail != NULL) {
			extent_deactivate(tsdn, arena, ecache, trail);
		}
		return edata;
	} else {
		/*
		 * We should have picked an extent that was large enough to
		 * fulfill our allocation request.
		 */
		assert(result == extent_split_interior_error);
		if (to_salvage != NULL) {
			extent_deregister(tsdn, to_salvage);
		}
		if (to_leak != NULL) {
			void *leak = edata_base_get(to_leak);
			extent_deregister_no_gdump_sub(tsdn, to_leak);
			extents_abandon_vm(tsdn, arena, ehooks, ecache, to_leak,
			    growing_retained);
			assert(extent_lock_edata_from_addr(tsdn, rtree_ctx, leak,
			    false) == NULL);
		}
		return NULL;
	}
	unreachable();
}

/*
 * Tries to satisfy the given allocation request by reusing one of the extents
 * in the given ecache_t.
 */
static edata_t *
extent_recycle(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, ecache_t *ecache,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(new_addr == NULL || !slab);
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	edata_t *edata = extent_recycle_extract(tsdn, arena, ehooks,
	    rtree_ctx, ecache, new_addr, size, pad, alignment, slab,
	    growing_retained);
	if (edata == NULL) {
		return NULL;
	}

	edata = extent_recycle_split(tsdn, arena, ehooks, rtree_ctx, ecache,
	    new_addr, size, pad, alignment, slab, szind, edata,
	    growing_retained);
	if (edata == NULL) {
		return NULL;
	}

	if (*commit && !edata_committed_get(edata)) {
		if (extent_commit_impl(tsdn, arena, ehooks, edata, 0,
		    edata_size_get(edata), growing_retained)) {
			extent_record(tsdn, arena, ehooks, ecache, edata,
			    growing_retained);
			return NULL;
		}
	}

	if (edata_committed_get(edata)) {
		*commit = true;
	}
	if (edata_zeroed_get(edata)) {
		*zero = true;
	}

	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, edata, alignment);
	}
	assert(edata_state_get(edata) == extent_state_active);
	if (slab) {
		edata_slab_set(edata, slab);
		extent_interior_register(tsdn, rtree_ctx, edata, szind);
	}

	if (*zero) {
		void *addr = edata_base_get(edata);
		if (!edata_zeroed_get(edata)) {
			size_t size = edata_size_get(edata);
			ehooks_zero(tsdn, ehooks, addr, size,
			    arena_ind_get(arena));
		}
	}
	return edata;
}

/*
 * If virtual memory is retained, create increasingly larger extents from which
 * to split requested extents in order to limit the total number of disjoint
 * virtual memory ranges retained by each arena.
 */
static edata_t *
extent_grow_retained(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    size_t size, size_t pad, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit) {
	malloc_mutex_assert_owner(tsdn, &arena->ecache_grow.mtx);
	assert(pad == 0 || !slab);
	assert(!*zero || !slab);

	size_t esize = size + pad;
	size_t alloc_size_min = esize + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size_min < esize) {
		goto label_err;
	}
	/*
	 * Find the next extent size in the series that would be large enough to
	 * satisfy this request.
	 */
	pszind_t egn_skip = 0;
	size_t alloc_size = sz_pind2sz(arena->ecache_grow.next + egn_skip);
	while (alloc_size < alloc_size_min) {
		egn_skip++;
		if (arena->ecache_grow.next + egn_skip >=
		    sz_psz2ind(SC_LARGE_MAXCLASS)) {
			/* Outside legal range. */
			goto label_err;
		}
		alloc_size = sz_pind2sz(arena->ecache_grow.next + egn_skip);
	}

	edata_t *edata = edata_cache_get(tsdn, &arena->edata_cache,
	    arena->base);
	if (edata == NULL) {
		goto label_err;
	}
	bool zeroed = false;
	bool committed = false;

	void *ptr = ehooks_alloc(tsdn, ehooks, NULL, alloc_size, PAGE, &zeroed,
	    &committed, arena_ind_get(arena));

	edata_init(edata, arena_ind_get(arena), ptr, alloc_size, false,
	    SC_NSIZES, arena_extent_sn_next(arena), extent_state_active, zeroed,
	    committed, true, EXTENT_IS_HEAD);
	if (ptr == NULL) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
		goto label_err;
	}

	if (extent_register_no_gdump_add(tsdn, edata)) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
		goto label_err;
	}

	if (edata_zeroed_get(edata) && edata_committed_get(edata)) {
		*zero = true;
	}
	if (edata_committed_get(edata)) {
		*commit = true;
	}

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	edata_t *lead;
	edata_t *trail;
	edata_t *to_leak;
	edata_t *to_salvage;
	extent_split_interior_result_t result = extent_split_interior(tsdn,
	    arena, ehooks, rtree_ctx, &edata, &lead, &trail, &to_leak,
	    &to_salvage, NULL, size, pad, alignment, slab, szind, true);

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_record(tsdn, arena, ehooks,
			    &arena->ecache_retained, lead, true);
		}
		if (trail != NULL) {
			extent_record(tsdn, arena, ehooks,
			    &arena->ecache_retained, trail, true);
		}
	} else {
		/*
		 * We should have allocated a sufficiently large extent; the
		 * cant_alloc case should not occur.
		 */
		assert(result == extent_split_interior_error);
		if (to_salvage != NULL) {
			if (config_prof) {
				extent_gdump_add(tsdn, to_salvage);
			}
			extent_record(tsdn, arena, ehooks,
			    &arena->ecache_retained, to_salvage, true);
		}
		if (to_leak != NULL) {
			extent_deregister_no_gdump_sub(tsdn, to_leak);
			extents_abandon_vm(tsdn, arena, ehooks,
			    &arena->ecache_retained, to_leak, true);
		}
		goto label_err;
	}

	if (*commit && !edata_committed_get(edata)) {
		if (extent_commit_impl(tsdn, arena, ehooks, edata, 0,
		    edata_size_get(edata), true)) {
			extent_record(tsdn, arena, ehooks,
			    &arena->ecache_retained, edata, true);
			goto label_err;
		}
		/* A successful commit should return zeroed memory. */
		if (config_debug) {
			void *addr = edata_addr_get(edata);
			size_t *p = (size_t *)(uintptr_t)addr;
			/* Check the first page only. */
			for (size_t i = 0; i < PAGE / sizeof(size_t); i++) {
				assert(p[i] == 0);
			}
		}
	}

	/*
	 * Increment extent_grow_next if doing so wouldn't exceed the allowed
	 * range.
	 */
	if (arena->ecache_grow.next + egn_skip + 1 <=
	    arena->ecache_grow.limit) {
		arena->ecache_grow.next += egn_skip + 1;
	} else {
		arena->ecache_grow.next = arena->ecache_grow.limit;
	}
	/* All opportunities for failure are past. */
	malloc_mutex_unlock(tsdn, &arena->ecache_grow.mtx);

	if (config_prof) {
		/* Adjust gdump stats now that extent is final size. */
		extent_gdump_add(tsdn, edata);
	}
	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, edata, alignment);
	}
	if (slab) {
		rtree_ctx_t rtree_ctx_fallback;
		rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn,
		    &rtree_ctx_fallback);

		edata_slab_set(edata, true);
		extent_interior_register(tsdn, rtree_ctx, edata, szind);
	}
	if (*zero && !edata_zeroed_get(edata)) {
		void *addr = edata_base_get(edata);
		size_t size = edata_size_get(edata);
		ehooks_zero(tsdn, ehooks, addr, size, arena_ind_get(arena));
	}

	return edata;
label_err:
	malloc_mutex_unlock(tsdn, &arena->ecache_grow.mtx);
	return NULL;
}

static edata_t *
extent_alloc_retained(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	assert(size != 0);
	assert(alignment != 0);

	malloc_mutex_lock(tsdn, &arena->ecache_grow.mtx);

	edata_t *edata = extent_recycle(tsdn, arena, ehooks,
	    &arena->ecache_retained, new_addr, size, pad, alignment, slab,
	    szind, zero, commit, true);
	if (edata != NULL) {
		malloc_mutex_unlock(tsdn, &arena->ecache_grow.mtx);
		if (config_prof) {
			extent_gdump_add(tsdn, edata);
		}
	} else if (opt_retain && new_addr == NULL) {
		edata = extent_grow_retained(tsdn, arena, ehooks, size, pad,
		    alignment, slab, szind, zero, commit);
		/* extent_grow_retained() always releases extent_grow_mtx. */
	} else {
		malloc_mutex_unlock(tsdn, &arena->ecache_grow.mtx);
	}
	malloc_mutex_assert_not_owner(tsdn, &arena->ecache_grow.mtx);

	return edata;
}

static edata_t *
extent_alloc_wrapper_hard(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	size_t esize = size + pad;
	edata_t *edata = edata_cache_get(tsdn, &arena->edata_cache,
	    arena->base);
	if (edata == NULL) {
		return NULL;
	}
	size_t palignment = ALIGNMENT_CEILING(alignment, PAGE);
	void *addr = ehooks_alloc(tsdn, ehooks, new_addr, esize, palignment,
	    zero, commit, arena_ind_get(arena));
	if (addr == NULL) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
		return NULL;
	}
	edata_init(edata, arena_ind_get(arena), addr, esize, slab, szind,
	    arena_extent_sn_next(arena), extent_state_active, *zero, *commit,
	    true, EXTENT_NOT_HEAD);
	if (pad != 0) {
		extent_addr_randomize(tsdn, arena, edata, alignment);
	}
	if (extent_register(tsdn, edata)) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
		return NULL;
	}

	return edata;
}

edata_t *
extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t pad, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_t *edata = extent_alloc_retained(tsdn, arena, ehooks, new_addr,
	    size, pad, alignment, slab, szind, zero, commit);
	if (edata == NULL) {
		if (opt_retain && new_addr != NULL) {
			/*
			 * When retain is enabled and new_addr is set, we do not
			 * attempt extent_alloc_wrapper_hard which does mmap
			 * that is very unlikely to succeed (unless it happens
			 * to be at the end).
			 */
			return NULL;
		}
		edata = extent_alloc_wrapper_hard(tsdn, arena, ehooks,
		    new_addr, size, pad, alignment, slab, szind, zero, commit);
	}

	assert(edata == NULL || edata_dumpable_get(edata));
	return edata;
}

static bool
extent_can_coalesce(arena_t *arena, ecache_t *ecache, const edata_t *inner,
    const edata_t *outer) {
	assert(edata_arena_ind_get(inner) == arena_ind_get(arena));
	if (edata_arena_ind_get(outer) != arena_ind_get(arena)) {
		return false;
	}

	assert(edata_state_get(inner) == extent_state_active);
	if (edata_state_get(outer) != ecache->state) {
		return false;
	}

	if (edata_committed_get(inner) != edata_committed_get(outer)) {
		return false;
	}

	return true;
}

static bool
extent_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *inner, edata_t *outer, bool forward,
    bool growing_retained) {
	assert(extent_can_coalesce(arena, ecache, inner, outer));

	extent_activate_locked(tsdn, arena, ecache, outer);

	malloc_mutex_unlock(tsdn, &ecache->mtx);
	bool err = extent_merge_impl(tsdn, arena, ehooks,
	    forward ? inner : outer, forward ? outer : inner, growing_retained);
	malloc_mutex_lock(tsdn, &ecache->mtx);

	if (err) {
		extent_deactivate_locked(tsdn, arena, ecache, outer);
	}

	return err;
}

static edata_t *
extent_try_coalesce_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, edata_t *edata, bool *coalesced,
    bool growing_retained, bool inactive_only) {
	/*
	 * We avoid checking / locking inactive neighbors for large size
	 * classes, since they are eagerly coalesced on deallocation which can
	 * cause lock contention.
	 */
	/*
	 * Continue attempting to coalesce until failure, to protect against
	 * races with other threads that are thwarted by this one.
	 */
	bool again;
	do {
		again = false;

		/* Try to coalesce forward. */
		edata_t *next = extent_lock_edata_from_addr(tsdn, rtree_ctx,
		    edata_past_get(edata), inactive_only);
		if (next != NULL) {
			/*
			 * ecache->mtx only protects against races for
			 * like-state extents, so call extent_can_coalesce()
			 * before releasing next's pool lock.
			 */
			bool can_coalesce = extent_can_coalesce(arena, ecache,
			    edata, next);

			extent_unlock_edata(tsdn, next);

			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    ehooks, ecache, edata, next, true,
			    growing_retained)) {
				if (ecache->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return edata;
				}
				again = true;
			}
		}

		/* Try to coalesce backward. */
		edata_t *prev = extent_lock_edata_from_addr(tsdn, rtree_ctx,
		    edata_before_get(edata), inactive_only);
		if (prev != NULL) {
			bool can_coalesce = extent_can_coalesce(arena, ecache,
			    edata, prev);
			extent_unlock_edata(tsdn, prev);

			if (can_coalesce && !extent_coalesce(tsdn, arena,
			    ehooks, ecache, edata, prev, false,
			    growing_retained)) {
				edata = prev;
				if (ecache->delay_coalesce) {
					/* Do minimal coalescing. */
					*coalesced = true;
					return edata;
				}
				again = true;
			}
		}
	} while (again);

	if (ecache->delay_coalesce) {
		*coalesced = false;
	}
	return edata;
}

static edata_t *
extent_try_coalesce(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, edata_t *edata, bool *coalesced,
    bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, arena, ehooks, rtree_ctx, ecache,
	    edata, coalesced, growing_retained, false);
}

static edata_t *
extent_try_coalesce_large(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    rtree_ctx_t *rtree_ctx, ecache_t *ecache, edata_t *edata, bool *coalesced,
    bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, arena, ehooks, rtree_ctx, ecache,
	    edata, coalesced, growing_retained, true);
}

/*
 * Does the metadata management portions of putting an unused extent into the
 * given ecache_t (coalesces, deregisters slab interiors, the heap operations).
 */
static void
extent_record(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, ecache_t *ecache,
    edata_t *edata, bool growing_retained) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	assert((ecache->state != extent_state_dirty &&
	    ecache->state != extent_state_muzzy) ||
	    !edata_zeroed_get(edata));

	malloc_mutex_lock(tsdn, &ecache->mtx);

	edata_szind_set(edata, SC_NSIZES);
	if (edata_slab_get(edata)) {
		extent_interior_deregister(tsdn, rtree_ctx, edata);
		edata_slab_set(edata, false);
	}

	assert(rtree_edata_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata), true) == edata);

	if (!ecache->delay_coalesce) {
		edata = extent_try_coalesce(tsdn, arena, ehooks, rtree_ctx,
		    ecache, edata, NULL, growing_retained);
	} else if (edata_size_get(edata) >= SC_LARGE_MINCLASS) {
		assert(ecache == &arena->ecache_dirty);
		/* Always coalesce large extents eagerly. */
		bool coalesced;
		do {
			assert(edata_state_get(edata) == extent_state_active);
			edata = extent_try_coalesce_large(tsdn, arena, ehooks,
			    rtree_ctx, ecache, edata, &coalesced,
			    growing_retained);
		} while (coalesced);
		if (edata_size_get(edata) >= oversize_threshold) {
			/* Shortcut to purge the oversize extent eagerly. */
			malloc_mutex_unlock(tsdn, &ecache->mtx);
			arena_decay_extent(tsdn, arena, ehooks, edata);
			return;
		}
	}
	extent_deactivate_locked(tsdn, arena, ecache, edata);

	malloc_mutex_unlock(tsdn, &ecache->mtx);
}

void
extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, edata_t *edata) {
	ehooks_t *ehooks = arena_get_ehooks(arena);

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (extent_register(tsdn, edata)) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
		return;
	}
	extent_dalloc_wrapper(tsdn, arena, ehooks, edata);
}

static bool
extent_dalloc_wrapper_try(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata) {
	bool err;

	assert(edata_base_get(edata) != NULL);
	assert(edata_size_get(edata) != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_addr_set(edata, edata_base_get(edata));

	/* Try to deallocate. */
	err = ehooks_dalloc(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), edata_committed_get(edata),
	    arena_ind_get(arena));

	if (!err) {
		edata_cache_put(tsdn, &arena->edata_cache, edata);
	}

	return err;
}

void
extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata) {
	assert(edata_dumpable_get(edata));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* Avoid calling the default extent_dalloc unless have to. */
	if (!ehooks_dalloc_will_fail(ehooks)) {
		/*
		 * Deregister first to avoid a race with other allocating
		 * threads, and reregister if deallocation fails.
		 */
		extent_deregister(tsdn, edata);
		if (!extent_dalloc_wrapper_try(tsdn, arena, ehooks, edata)) {
			return;
		}
		extent_reregister(tsdn, edata);
	}

	/* Try to decommit; purge if that fails. */
	bool zeroed;
	if (!edata_committed_get(edata)) {
		zeroed = true;
	} else if (!extent_decommit_wrapper(tsdn, arena, ehooks, edata, 0,
	    edata_size_get(edata))) {
		zeroed = true;
	} else if (!ehooks_purge_forced(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), 0, edata_size_get(edata),
	    arena_ind_get(arena))) {
		zeroed = true;
	} else if (edata_state_get(edata) == extent_state_muzzy ||
	    !ehooks_purge_lazy(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), 0, edata_size_get(edata),
	    arena_ind_get(arena))) {
		zeroed = false;
	} else {
		zeroed = false;
	}
	edata_zeroed_set(edata, zeroed);

	if (config_prof) {
		extent_gdump_sub(tsdn, edata);
	}

	extent_record(tsdn, arena, ehooks, &arena->ecache_retained, edata,
	    false);
}

void
extent_destroy_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata) {
	assert(edata_base_get(edata) != NULL);
	assert(edata_size_get(edata) != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	/* Deregister first to avoid a race with other allocating threads. */
	extent_deregister(tsdn, edata);

	edata_addr_set(edata, edata_base_get(edata));

	/* Try to destroy; silently fail otherwise. */
	ehooks_destroy(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), edata_committed_get(edata),
	    arena_ind_get(arena));

	edata_cache_put(tsdn, &arena->edata_cache, edata);
}

static bool
extent_commit_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_commit(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length, arena_ind_get(arena));
	edata_committed_set(edata, edata_committed_get(edata) || !err);
	return err;
}

bool
extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset,
    size_t length) {
	return extent_commit_impl(tsdn, arena, ehooks, edata, offset, length,
	    false);
}

bool
extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	bool err = ehooks_decommit(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length, arena_ind_get(arena));
	edata_committed_set(edata, edata_committed_get(edata) && err);
	return err;
}

static bool
extent_purge_lazy_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_purge_lazy(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length, arena_ind_get(arena));
	return err;
}

bool
extent_purge_lazy_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length) {
	return extent_purge_lazy_impl(tsdn, arena, ehooks, edata, offset,
	    length, false);
}

static bool
extent_purge_forced_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_purge_forced(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length, arena_ind_get(arena));
	return err;
}

bool
extent_purge_forced_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length) {
	return extent_purge_forced_impl(tsdn, arena, ehooks, edata,
	    offset, length, false);
}

/*
 * Accepts the extent to split, and the characteristics of each side of the
 * split.  The 'a' parameters go with the 'lead' of the resulting pair of
 * extents (the lower addressed portion of the split), and the 'b' parameters go
 * with the trail (the higher addressed portion).  This makes 'extent' the lead,
 * and returns the trail (except in case of error).
 */
static edata_t *
extent_split_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b, bool growing_retained) {
	assert(edata_size_get(edata) == size_a + size_b);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);

	if (ehooks_split_will_fail(ehooks)) {
		return NULL;
	}

	edata_t *trail = edata_cache_get(tsdn, &arena->edata_cache,
	    arena->base);
	if (trail == NULL) {
		goto label_error_a;
	}

	edata_init(trail, arena_ind_get(arena),
	    (void *)((uintptr_t)edata_base_get(edata) + size_a), size_b,
	    slab_b, szind_b, edata_sn_get(edata), edata_state_get(edata),
	    edata_zeroed_get(edata), edata_committed_get(edata),
	    edata_dumpable_get(edata), EXTENT_NOT_HEAD);

	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *lead_elm_a, *lead_elm_b;
	{
		edata_t lead;

		edata_init(&lead, arena_ind_get(arena),
		    edata_addr_get(edata), size_a,
		    slab_a, szind_a, edata_sn_get(edata),
		    edata_state_get(edata), edata_zeroed_get(edata),
		    edata_committed_get(edata), edata_dumpable_get(edata),
		    EXTENT_NOT_HEAD);

		extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, &lead, false,
		    true, &lead_elm_a, &lead_elm_b);
	}
	rtree_leaf_elm_t *trail_elm_a, *trail_elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, trail, false, true,
	    &trail_elm_a, &trail_elm_b);

	if (lead_elm_a == NULL || lead_elm_b == NULL || trail_elm_a == NULL
	    || trail_elm_b == NULL) {
		goto label_error_b;
	}

	extent_lock_edata2(tsdn, edata, trail);

	bool err = ehooks_split(tsdn, ehooks, edata_base_get(edata),
	    size_a + size_b, size_a, size_b, edata_committed_get(edata),
	    arena_ind_get(arena));

	if (err) {
		goto label_error_c;
	}

	edata_size_set(edata, size_a);
	edata_szind_set(edata, szind_a);

	extent_rtree_write_acquired(tsdn, lead_elm_a, lead_elm_b, edata,
	    szind_a, slab_a);
	extent_rtree_write_acquired(tsdn, trail_elm_a, trail_elm_b, trail,
	    szind_b, slab_b);

	extent_unlock_edata2(tsdn, edata, trail);

	return trail;
label_error_c:
	extent_unlock_edata2(tsdn, edata, trail);
label_error_b:
	edata_cache_put(tsdn, &arena->edata_cache, trail);
label_error_a:
	return NULL;
}

edata_t *
extent_split_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b) {
	return extent_split_impl(tsdn, arena, ehooks, edata, size_a, szind_a,
	    slab_a, size_b, szind_b, slab_b, false);
}

static bool
extent_merge_impl(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks, edata_t *a,
    edata_t *b, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(edata_base_get(a) < edata_base_get(b));

	bool err = ehooks_merge(tsdn, ehooks, edata_base_get(a),
	    edata_size_get(a), edata_is_head_get(a), edata_base_get(b),
	    edata_size_get(b), edata_is_head_get(b), edata_committed_get(a),
	    arena_ind_get(arena));

	if (err) {
		return true;
	}

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than extent_{,de}register() to do things in the right order.
	 */
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_leaf_elm_t *a_elm_a, *a_elm_b, *b_elm_a, *b_elm_b;
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, a, true, false, &a_elm_a,
	    &a_elm_b);
	extent_rtree_leaf_elms_lookup(tsdn, rtree_ctx, b, true, false, &b_elm_a,
	    &b_elm_b);

	extent_lock_edata2(tsdn, a, b);

	if (a_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, a_elm_b, NULL,
		    SC_NSIZES, false);
	}
	if (b_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &extents_rtree, b_elm_a, NULL,
		    SC_NSIZES, false);
	} else {
		b_elm_b = b_elm_a;
	}

	edata_size_set(a, edata_size_get(a) + edata_size_get(b));
	edata_szind_set(a, SC_NSIZES);
	edata_sn_set(a, (edata_sn_get(a) < edata_sn_get(b)) ?
	    edata_sn_get(a) : edata_sn_get(b));
	edata_zeroed_set(a, edata_zeroed_get(a) && edata_zeroed_get(b));

	extent_rtree_write_acquired(tsdn, a_elm_a, b_elm_b, a, SC_NSIZES,
	    false);

	extent_unlock_edata2(tsdn, a, b);

	/*
	 * If we got here, we merged the extents; so they must be from the same
	 * arena (i.e. this one).
	 */
	assert(edata_arena_ind_get(b) == arena_ind_get(arena));
	edata_cache_put(tsdn, &arena->edata_cache, b);

	return false;
}

bool
extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena, ehooks_t *ehooks,
    edata_t *a, edata_t *b) {
	return extent_merge_impl(tsdn, arena, ehooks, a, b, false);
}

bool
extent_boot(void) {
	if (rtree_new(&extents_rtree, true)) {
		return true;
	}

	if (mutex_pool_init(&extent_mutex_pool, "extent_mutex_pool",
	    WITNESS_RANK_EXTENT_POOL)) {
		return true;
	}

	if (have_dss) {
		extent_dss_boot();
	}

	return false;
}
