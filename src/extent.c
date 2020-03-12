#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/mutex.h"

/******************************************************************************/
/* Data. */

size_t opt_lg_extent_max_active_fit = LG_EXTENT_MAX_ACTIVE_FIT_DEFAULT;

static bool extent_commit_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length, bool growing_retained);
static bool extent_purge_lazy_impl(tsdn_t *tsdn, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained);
static bool extent_purge_forced_impl(tsdn_t *tsdn, ehooks_t *ehooks,
    edata_t *edata, size_t offset, size_t length, bool growing_retained);
static edata_t *extent_split_impl(tsdn_t *tsdn, edata_cache_t *edata_cache,
    ehooks_t *ehooks, edata_t *edata, size_t size_a, szind_t szind_a,
    bool slab_a, size_t size_b, szind_t szind_b, bool slab_b,
    bool growing_retained);
static bool extent_merge_impl(tsdn_t *tsdn, ehooks_t *ehooks,
    edata_cache_t *edata_cache, edata_t *a, edata_t *b, bool growing_retained);

/* Used exclusively for gdump triggering. */
static atomic_zu_t curpages;
static atomic_zu_t highpages;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void extent_deregister(tsdn_t *tsdn, edata_t *edata);
static edata_t *extent_recycle(tsdn_t *tsdn, pa_shard_t *shard,
    ehooks_t *ehooks, ecache_t *ecache, void *new_addr, size_t usize,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit,
    bool growing_retained);
static edata_t *extent_try_coalesce(tsdn_t *tsdn, edata_cache_t *edata_cache,
    ehooks_t *ehooks, ecache_t *ecache, edata_t *edata, bool *coalesced,
    bool growing_retained);
static void extent_record(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool growing_retained);
static edata_t *extent_alloc_retained(tsdn_t *tsdn, pa_shard_t *shard,
    ehooks_t *ehooks, void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit);

/******************************************************************************/

static bool
extent_try_delayed_coalesce(tsdn_t *tsdn, edata_cache_t *edata_cache,
    ehooks_t *ehooks, ecache_t *ecache, edata_t *edata) {
	edata_state_set(edata, extent_state_active);
	bool coalesced;
	edata = extent_try_coalesce(tsdn, edata_cache, ehooks, ecache, edata,
	    &coalesced, false);
	edata_state_set(edata, ecache->state);

	if (!coalesced) {
		return true;
	}
	eset_insert(&ecache->eset, edata);
	return false;
}

edata_t *
ecache_alloc(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, bool *zero) {
	assert(size != 0);
	assert(alignment != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	bool commit = true;
	edata_t *edata = extent_recycle(tsdn, shard, ehooks, ecache,
	    new_addr, size, alignment, slab, szind, zero, &commit, false);
	assert(edata == NULL || edata_dumpable_get(edata));
	return edata;
}

edata_t *
ecache_alloc_grow(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, bool *zero) {
	assert(size != 0);
	assert(alignment != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	bool commit = true;
	edata_t *edata = extent_alloc_retained(tsdn, shard, ehooks, new_addr,
	    size, alignment, slab, szind, zero, &commit);
	if (edata == NULL) {
		if (opt_retain && new_addr != NULL) {
			/*
			 * When retain is enabled and new_addr is set, we do not
			 * attempt extent_alloc_wrapper which does mmap that is
			 * very unlikely to succeed (unless it happens to be at
			 * the end).
			 */
			return NULL;
		}
		edata = extent_alloc_wrapper(tsdn, shard, ehooks, new_addr,
		    size, alignment, slab, szind, zero, &commit);
	}

	assert(edata == NULL || edata_dumpable_get(edata));
	return edata;
}

void
ecache_dalloc(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata) {
	assert(edata_base_get(edata) != NULL);
	assert(edata_size_get(edata) != 0);
	assert(edata_dumpable_get(edata));
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_addr_set(edata, edata_base_get(edata));
	edata_zeroed_set(edata, false);

	extent_record(tsdn, shard, ehooks, ecache, edata, false);
}

edata_t *
ecache_evict(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, size_t npages_min) {
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
		if (extent_try_delayed_coalesce(tsdn, &shard->edata_cache,
		    ehooks, ecache, edata)) {
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
extents_abandon_vm(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool growing_retained) {
	size_t sz = edata_size_get(edata);
	if (config_stats) {
		atomic_fetch_add_zu(&shard->stats->abandoned_vm, sz,
		    ATOMIC_RELAXED);
	}
	/*
	 * Leak extent after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (ecache->state == extent_state_dirty) {
		if (extent_purge_lazy_impl(tsdn, ehooks, edata, 0, sz,
		    growing_retained)) {
			extent_purge_forced_impl(tsdn, ehooks, edata, 0,
			    edata_size_get(edata), growing_retained);
		}
	}
	edata_cache_put(tsdn, &shard->edata_cache, edata);
}

static void
extent_deactivate_locked(tsdn_t *tsdn, ecache_t *ecache, edata_t *edata) {
	assert(edata_arena_ind_get(edata) == ecache_ind_get(ecache));
	assert(edata_state_get(edata) == extent_state_active);

	edata_state_set(edata, ecache->state);
	eset_insert(&ecache->eset, edata);
}

static void
extent_deactivate(tsdn_t *tsdn, ecache_t *ecache, edata_t *edata) {
	malloc_mutex_lock(tsdn, &ecache->mtx);
	extent_deactivate_locked(tsdn, ecache, edata);
	malloc_mutex_unlock(tsdn, &ecache->mtx);
}

static void
extent_activate_locked(tsdn_t *tsdn, ecache_t *ecache, edata_t *edata) {
	assert(edata_arena_ind_get(edata) == ecache_ind_get(ecache));
	assert(edata_state_get(edata) == ecache->state);

	eset_remove(&ecache->eset, edata);
	edata_state_set(edata, extent_state_active);
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
	/*
	 * We need to hold the lock to protect against a concurrent coalesce
	 * operation that sees us in a partial state.
	 */
	emap_lock_edata(tsdn, &emap_global, edata);

	szind_t szind = edata_szind_get_maybe_invalid(edata);
	bool slab = edata_slab_get(edata);

	if (emap_register_boundary(tsdn, &emap_global, edata, szind, slab)) {
		emap_unlock_edata(tsdn, &emap_global, edata);
		return true;
	}

	if (slab) {
		emap_register_interior(tsdn, &emap_global, edata, szind);
	}

	emap_unlock_edata(tsdn, &emap_global, edata);

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
 * Removes all pointers to the given extent from the global rtree.
 */
static void
extent_deregister_impl(tsdn_t *tsdn, edata_t *edata, bool gdump) {
	emap_lock_edata(tsdn, &emap_global, edata);
	emap_deregister_boundary(tsdn, &emap_global, edata);
	if (edata_slab_get(edata)) {
		emap_deregister_interior(tsdn, &emap_global, edata);
		edata_slab_set(edata, false);
	}
	emap_unlock_edata(tsdn, &emap_global, edata);

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
extent_recycle_extract(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t size, size_t alignment, bool slab,
    bool growing_retained) {
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
		assert(alignment <= PAGE);
	}

	malloc_mutex_lock(tsdn, &ecache->mtx);
	edata_t *edata;
	if (new_addr != NULL) {
		edata = emap_lock_edata_from_addr(tsdn, &emap_global, new_addr,
		    false);
		if (edata != NULL) {
			/*
			 * We might null-out edata to report an error, but we
			 * still need to unlock the associated mutex after.
			 */
			edata_t *unlock_edata = edata;
			assert(edata_base_get(edata) == new_addr);
			if (edata_arena_ind_get(edata) != ecache_ind_get(ecache)
			    || edata_size_get(edata) < size
			    || edata_state_get(edata)
			    != ecache->state) {
				edata = NULL;
			}
			emap_unlock_edata(tsdn, &emap_global, unlock_edata);
		}
	} else {
		edata = eset_fit(&ecache->eset, size, alignment,
		    ecache->delay_coalesce);
	}
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &ecache->mtx);
		return NULL;
	}

	extent_activate_locked(tsdn, ecache, edata);
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
extent_split_interior(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    /* The result of splitting, in case of success. */
    edata_t **edata, edata_t **lead, edata_t **trail,
    /* The mess to clean up, in case of error. */
    edata_t **to_leak, edata_t **to_salvage,
    void *new_addr, size_t size, size_t alignment, bool slab, szind_t szind,
    bool growing_retained) {
	size_t leadsize = ALIGNMENT_CEILING((uintptr_t)edata_base_get(*edata),
	    PAGE_CEILING(alignment)) - (uintptr_t)edata_base_get(*edata);
	assert(new_addr == NULL || leadsize == 0);
	if (edata_size_get(*edata) < leadsize + size) {
		return extent_split_interior_cant_alloc;
	}
	size_t trailsize = edata_size_get(*edata) - leadsize - size;

	*lead = NULL;
	*trail = NULL;
	*to_leak = NULL;
	*to_salvage = NULL;

	/* Split the lead. */
	if (leadsize != 0) {
		*lead = *edata;
		*edata = extent_split_impl(tsdn, &shard->edata_cache, ehooks,
		    *lead, leadsize, SC_NSIZES, false, size + trailsize, szind,
		    slab, growing_retained);
		if (*edata == NULL) {
			*to_leak = *lead;
			*lead = NULL;
			return extent_split_interior_error;
		}
	}

	/* Split the trail. */
	if (trailsize != 0) {
		*trail = extent_split_impl(tsdn, &shard->edata_cache, ehooks,
		    *edata, size, szind, slab, trailsize, SC_NSIZES, false,
		    growing_retained);
		if (*trail == NULL) {
			*to_leak = *edata;
			*to_salvage = *lead;
			*lead = NULL;
			*edata = NULL;
			return extent_split_interior_error;
		}
	}

	if (leadsize == 0 && trailsize == 0) {
		emap_remap(tsdn, &emap_global, *edata, szind, slab);
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
extent_recycle_split(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, edata_t *edata, bool growing_retained) {
	edata_t *lead;
	edata_t *trail;
	edata_t *to_leak JEMALLOC_CC_SILENCE_INIT(NULL);
	edata_t *to_salvage JEMALLOC_CC_SILENCE_INIT(NULL);

	extent_split_interior_result_t result = extent_split_interior(
	    tsdn, shard, ehooks, &edata, &lead, &trail, &to_leak, &to_salvage,
	    new_addr, size, alignment, slab, szind, growing_retained);

	if (!maps_coalesce && result != extent_split_interior_ok
	    && !opt_retain) {
		/*
		 * Split isn't supported (implies Windows w/o retain).  Avoid
		 * leaking the extent.
		 */
		assert(to_leak != NULL && lead == NULL && trail == NULL);
		extent_deactivate(tsdn, ecache, to_leak);
		return NULL;
	}

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_deactivate(tsdn, ecache, lead);
		}
		if (trail != NULL) {
			extent_deactivate(tsdn, ecache, trail);
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
			extents_abandon_vm(tsdn, shard, ehooks, ecache, to_leak,
			    growing_retained);
			assert(emap_lock_edata_from_addr(tsdn, &emap_global,
			    leak, false) == NULL);
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
extent_recycle(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(new_addr == NULL || !slab);
	assert(!*zero || !slab);

	edata_t *edata = extent_recycle_extract(tsdn, shard, ehooks, ecache,
	    new_addr, size, alignment, slab, growing_retained);
	if (edata == NULL) {
		return NULL;
	}

	edata = extent_recycle_split(tsdn, shard, ehooks, ecache, new_addr,
	    size, alignment, slab, szind, edata, growing_retained);
	if (edata == NULL) {
		return NULL;
	}

	if (*commit && !edata_committed_get(edata)) {
		if (extent_commit_impl(tsdn, ehooks, edata, 0,
		    edata_size_get(edata), growing_retained)) {
			extent_record(tsdn, shard, ehooks, ecache, edata,
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

	assert(edata_state_get(edata) == extent_state_active);
	if (slab) {
		edata_slab_set(edata, slab);
		emap_register_interior(tsdn, &emap_global, edata, szind);
	}

	if (*zero) {
		void *addr = edata_base_get(edata);
		if (!edata_zeroed_get(edata)) {
			size_t size = edata_size_get(edata);
			ehooks_zero(tsdn, ehooks, addr, size);
		}
	}
	return edata;
}

/*
 * If virtual memory is retained, create increasingly larger extents from which
 * to split requested extents in order to limit the total number of disjoint
 * virtual memory ranges retained by each shard.
 */
static edata_t *
extent_grow_retained(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    size_t size, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit) {
	malloc_mutex_assert_owner(tsdn, &shard->ecache_grow.mtx);
	assert(!*zero || !slab);

	size_t alloc_size_min = size + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size_min < size) {
		goto label_err;
	}
	/*
	 * Find the next extent size in the series that would be large enough to
	 * satisfy this request.
	 */
	pszind_t egn_skip = 0;
	size_t alloc_size = sz_pind2sz(shard->ecache_grow.next + egn_skip);
	while (alloc_size < alloc_size_min) {
		egn_skip++;
		if (shard->ecache_grow.next + egn_skip >=
		    sz_psz2ind(SC_LARGE_MAXCLASS)) {
			/* Outside legal range. */
			goto label_err;
		}
		alloc_size = sz_pind2sz(
		    shard->ecache_grow.next + egn_skip);
	}

	edata_t *edata = edata_cache_get(tsdn, &shard->edata_cache);
	if (edata == NULL) {
		goto label_err;
	}
	bool zeroed = false;
	bool committed = false;

	void *ptr = ehooks_alloc(tsdn, ehooks, NULL, alloc_size, PAGE, &zeroed,
	    &committed);

	if (ptr == NULL) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
		goto label_err;
	}

	edata_init(edata, ecache_ind_get(&shard->ecache_retained), ptr,
	    alloc_size, false, SC_NSIZES, pa_shard_extent_sn_next(shard),
	    extent_state_active, zeroed, committed, true, EXTENT_IS_HEAD);

	if (extent_register_no_gdump_add(tsdn, edata)) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
		goto label_err;
	}

	if (edata_zeroed_get(edata) && edata_committed_get(edata)) {
		*zero = true;
	}
	if (edata_committed_get(edata)) {
		*commit = true;
	}

	edata_t *lead;
	edata_t *trail;
	edata_t *to_leak JEMALLOC_CC_SILENCE_INIT(NULL);
	edata_t *to_salvage JEMALLOC_CC_SILENCE_INIT(NULL);

	extent_split_interior_result_t result = extent_split_interior(tsdn,
	    shard, ehooks, &edata, &lead, &trail, &to_leak, &to_salvage, NULL,
	    size, alignment, slab, szind, true);

	if (result == extent_split_interior_ok) {
		if (lead != NULL) {
			extent_record(tsdn, shard, ehooks,
			    &shard->ecache_retained, lead, true);
		}
		if (trail != NULL) {
			extent_record(tsdn, shard, ehooks,
			    &shard->ecache_retained, trail, true);
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
			extent_record(tsdn, shard, ehooks,
			    &shard->ecache_retained, to_salvage, true);
		}
		if (to_leak != NULL) {
			extent_deregister_no_gdump_sub(tsdn, to_leak);
			extents_abandon_vm(tsdn, shard, ehooks,
			    &shard->ecache_retained, to_leak, true);
		}
		goto label_err;
	}

	if (*commit && !edata_committed_get(edata)) {
		if (extent_commit_impl(tsdn, ehooks, edata, 0,
		    edata_size_get(edata), true)) {
			extent_record(tsdn, shard, ehooks,
			    &shard->ecache_retained, edata, true);
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
	if (shard->ecache_grow.next + egn_skip + 1 <=
	    shard->ecache_grow.limit) {
		shard->ecache_grow.next += egn_skip + 1;
	} else {
		shard->ecache_grow.next = shard->ecache_grow.limit;
	}
	/* All opportunities for failure are past. */
	malloc_mutex_unlock(tsdn, &shard->ecache_grow.mtx);

	if (config_prof) {
		/* Adjust gdump stats now that extent is final size. */
		extent_gdump_add(tsdn, edata);
	}
	if (slab) {
		edata_slab_set(edata, true);
		emap_register_interior(tsdn, &emap_global, edata, szind);
	}
	if (*zero && !edata_zeroed_get(edata)) {
		void *addr = edata_base_get(edata);
		size_t size = edata_size_get(edata);
		ehooks_zero(tsdn, ehooks, addr, size);
	}

	return edata;
label_err:
	malloc_mutex_unlock(tsdn, &shard->ecache_grow.mtx);
	return NULL;
}

static edata_t *
extent_alloc_retained(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit) {
	assert(size != 0);
	assert(alignment != 0);

	malloc_mutex_lock(tsdn, &shard->ecache_grow.mtx);

	edata_t *edata = extent_recycle(tsdn, shard, ehooks,
	    &shard->ecache_retained, new_addr, size, alignment, slab,
	    szind, zero, commit, true);
	if (edata != NULL) {
		malloc_mutex_unlock(tsdn, &shard->ecache_grow.mtx);
		if (config_prof) {
			extent_gdump_add(tsdn, edata);
		}
	} else if (opt_retain && new_addr == NULL) {
		edata = extent_grow_retained(tsdn, shard, ehooks, size,
		    alignment, slab, szind, zero, commit);
		/* extent_grow_retained() always releases extent_grow_mtx. */
	} else {
		malloc_mutex_unlock(tsdn, &shard->ecache_grow.mtx);
	}
	malloc_mutex_assert_not_owner(tsdn, &shard->ecache_grow.mtx);

	return edata;
}

edata_t *
extent_alloc_wrapper(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    void *new_addr, size_t size, size_t alignment, bool slab,
    szind_t szind, bool *zero, bool *commit) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_t *edata = edata_cache_get(tsdn, &shard->edata_cache);
	if (edata == NULL) {
		return NULL;
	}
	size_t palignment = ALIGNMENT_CEILING(alignment, PAGE);
	void *addr = ehooks_alloc(tsdn, ehooks, new_addr, size, palignment,
	    zero, commit);
	if (addr == NULL) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
		return NULL;
	}
	edata_init(edata, ecache_ind_get(&shard->ecache_dirty), addr,
	    size, slab, szind, pa_shard_extent_sn_next(shard),
	    extent_state_active, *zero, *commit, true, EXTENT_NOT_HEAD);
	if (extent_register(tsdn, edata)) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
		return NULL;
	}

	return edata;
}

static bool
extent_can_coalesce(ecache_t *ecache, const edata_t *inner,
    const edata_t *outer) {
	assert(edata_arena_ind_get(inner) == ecache_ind_get(ecache));

	if (edata_arena_ind_get(inner) != edata_arena_ind_get(outer)) {
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
extent_coalesce(tsdn_t *tsdn, edata_cache_t *edata_cache, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *inner, edata_t *outer, bool forward,
    bool growing_retained) {
	assert(extent_can_coalesce(ecache, inner, outer));

	extent_activate_locked(tsdn, ecache, outer);

	malloc_mutex_unlock(tsdn, &ecache->mtx);
	bool err = extent_merge_impl(tsdn, ehooks, edata_cache,
	    forward ? inner : outer, forward ? outer : inner, growing_retained);
	malloc_mutex_lock(tsdn, &ecache->mtx);

	if (err) {
		extent_deactivate_locked(tsdn, ecache, outer);
	}

	return err;
}

static edata_t *
extent_try_coalesce_impl(tsdn_t *tsdn, edata_cache_t *edata_cache,
    ehooks_t *ehooks, ecache_t *ecache, edata_t *edata, bool *coalesced,
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
		edata_t *next = emap_lock_edata_from_addr(tsdn, &emap_global,
		    edata_past_get(edata), inactive_only);
		if (next != NULL) {
			/*
			 * ecache->mtx only protects against races for
			 * like-state extents, so call extent_can_coalesce()
			 * before releasing next's pool lock.
			 */
			bool can_coalesce = extent_can_coalesce(ecache,
			    edata, next);

			emap_unlock_edata(tsdn, &emap_global, next);

			if (can_coalesce && !extent_coalesce(tsdn, edata_cache,
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
		edata_t *prev = emap_lock_edata_from_addr(tsdn, &emap_global,
		    edata_before_get(edata), inactive_only);
		if (prev != NULL) {
			bool can_coalesce = extent_can_coalesce(ecache, edata,
			    prev);
			emap_unlock_edata(tsdn, &emap_global, prev);

			if (can_coalesce && !extent_coalesce(tsdn, edata_cache,
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
extent_try_coalesce(tsdn_t *tsdn, edata_cache_t *edata_cache, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool *coalesced, bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, edata_cache, ehooks,  ecache,
	    edata, coalesced, growing_retained, false);
}

static edata_t *
extent_try_coalesce_large(tsdn_t *tsdn, edata_cache_t *edata_cache,
    ehooks_t *ehooks, ecache_t *ecache, edata_t *edata, bool *coalesced,
    bool growing_retained) {
	return extent_try_coalesce_impl(tsdn, edata_cache, ehooks, ecache,
	    edata, coalesced, growing_retained, true);
}

/* Purge a single extent to retained / unmapped directly. */
static void
extent_maximally_purge(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    edata_t *edata) {
	size_t extent_size = edata_size_get(edata);
	extent_dalloc_wrapper(tsdn, shard, ehooks, edata);
	if (config_stats) {
		/* Update stats accordingly. */
		LOCKEDINT_MTX_LOCK(tsdn, *shard->stats_mtx);
		locked_inc_u64(tsdn,
		    LOCKEDINT_MTX(*shard->stats_mtx),
		    &shard->stats->decay_dirty.nmadvise, 1);
		locked_inc_u64(tsdn,
		    LOCKEDINT_MTX(*shard->stats_mtx),
		    &shard->stats->decay_dirty.purged,
		    extent_size >> LG_PAGE);
		LOCKEDINT_MTX_UNLOCK(tsdn, *shard->stats_mtx);
		atomic_fetch_sub_zu(&shard->stats->pa_mapped, extent_size,
		    ATOMIC_RELAXED);
	}
}


/*
 * Does the metadata management portions of putting an unused extent into the
 * given ecache_t (coalesces, deregisters slab interiors, the heap operations).
 */
static void
extent_record(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    ecache_t *ecache, edata_t *edata, bool growing_retained) {
	assert((ecache->state != extent_state_dirty &&
	    ecache->state != extent_state_muzzy) ||
	    !edata_zeroed_get(edata));

	malloc_mutex_lock(tsdn, &ecache->mtx);

	edata_szind_set(edata, SC_NSIZES);
	if (edata_slab_get(edata)) {
		emap_deregister_interior(tsdn, &emap_global, edata);
		edata_slab_set(edata, false);
	}

	emap_assert_mapped(tsdn, &emap_global, edata);

	if (!ecache->delay_coalesce) {
		edata = extent_try_coalesce(tsdn, &shard->edata_cache, ehooks,
		    ecache, edata, NULL, growing_retained);
	} else if (edata_size_get(edata) >= SC_LARGE_MINCLASS) {
		assert(ecache == &shard->ecache_dirty);
		/* Always coalesce large extents eagerly. */
		bool coalesced;
		do {
			assert(edata_state_get(edata) == extent_state_active);
			edata = extent_try_coalesce_large(tsdn,
			    &shard->edata_cache, ehooks, ecache, edata,
			    &coalesced, growing_retained);
		} while (coalesced);
		if (edata_size_get(edata) >= oversize_threshold &&
		    pa_shard_may_force_decay(shard)) {
			/* Shortcut to purge the oversize extent eagerly. */
			malloc_mutex_unlock(tsdn, &ecache->mtx);
			extent_maximally_purge(tsdn, shard, ehooks, edata);
			return;
		}
	}
	extent_deactivate_locked(tsdn, ecache, edata);

	malloc_mutex_unlock(tsdn, &ecache->mtx);
}

void
extent_dalloc_gap(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    edata_t *edata) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (extent_register(tsdn, edata)) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
		return;
	}
	extent_dalloc_wrapper(tsdn, shard, ehooks, edata);
}

static bool
extent_dalloc_wrapper_try(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
    edata_t *edata) {
	bool err;

	assert(edata_base_get(edata) != NULL);
	assert(edata_size_get(edata) != 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_addr_set(edata, edata_base_get(edata));

	/* Try to deallocate. */
	err = ehooks_dalloc(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), edata_committed_get(edata));

	if (!err) {
		edata_cache_put(tsdn, &shard->edata_cache, edata);
	}

	return err;
}

void
extent_dalloc_wrapper(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
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
		if (!extent_dalloc_wrapper_try(tsdn, shard, ehooks, edata)) {
			return;
		}
		extent_reregister(tsdn, edata);
	}

	/* Try to decommit; purge if that fails. */
	bool zeroed;
	if (!edata_committed_get(edata)) {
		zeroed = true;
	} else if (!extent_decommit_wrapper(tsdn, ehooks, edata, 0,
	    edata_size_get(edata))) {
		zeroed = true;
	} else if (!ehooks_purge_forced(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), 0, edata_size_get(edata))) {
		zeroed = true;
	} else if (edata_state_get(edata) == extent_state_muzzy ||
	    !ehooks_purge_lazy(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), 0, edata_size_get(edata))) {
		zeroed = false;
	} else {
		zeroed = false;
	}
	edata_zeroed_set(edata, zeroed);

	if (config_prof) {
		extent_gdump_sub(tsdn, edata);
	}

	extent_record(tsdn, shard, ehooks, &shard->ecache_retained, edata,
	    false);
}

void
extent_destroy_wrapper(tsdn_t *tsdn, pa_shard_t *shard, ehooks_t *ehooks,
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
	    edata_size_get(edata), edata_committed_get(edata));

	edata_cache_put(tsdn, &shard->edata_cache, edata);
}

static bool
extent_commit_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_commit(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length);
	edata_committed_set(edata, edata_committed_get(edata) || !err);
	return err;
}

bool
extent_commit_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length) {
	return extent_commit_impl(tsdn, ehooks, edata, offset, length,
	    false);
}

bool
extent_decommit_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	bool err = ehooks_decommit(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length);
	edata_committed_set(edata, edata_committed_get(edata) && err);
	return err;
}

static bool
extent_purge_lazy_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_purge_lazy(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length);
	return err;
}

bool
extent_purge_lazy_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length) {
	return extent_purge_lazy_impl(tsdn, ehooks, edata, offset,
	    length, false);
}

static bool
extent_purge_forced_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	bool err = ehooks_purge_forced(tsdn, ehooks, edata_base_get(edata),
	    edata_size_get(edata), offset, length);
	return err;
}

bool
extent_purge_forced_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    size_t offset, size_t length) {
	return extent_purge_forced_impl(tsdn, ehooks, edata, offset, length,
	    false);
}

/*
 * Accepts the extent to split, and the characteristics of each side of the
 * split.  The 'a' parameters go with the 'lead' of the resulting pair of
 * extents (the lower addressed portion of the split), and the 'b' parameters go
 * with the trail (the higher addressed portion).  This makes 'extent' the lead,
 * and returns the trail (except in case of error).
 */
static edata_t *
extent_split_impl(tsdn_t *tsdn, edata_cache_t *edata_cache, ehooks_t *ehooks,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b, bool growing_retained) {
	assert(edata_size_get(edata) == size_a + size_b);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);

	if (ehooks_split_will_fail(ehooks)) {
		return NULL;
	}

	edata_t *trail = edata_cache_get(tsdn, edata_cache);
	if (trail == NULL) {
		goto label_error_a;
	}

	emap_prepare_t prepare;
	bool err = emap_split_prepare(tsdn, &emap_global, &prepare, edata,
	    size_a, szind_a, slab_a, trail, size_b, szind_b, slab_b);
	if (err) {
		goto label_error_b;
	}

	emap_lock_edata2(tsdn, &emap_global, edata, trail);

	err = ehooks_split(tsdn, ehooks, edata_base_get(edata), size_a + size_b,
	    size_a, size_b, edata_committed_get(edata));

	if (err) {
		goto label_error_c;
	}

	emap_split_commit(tsdn, &emap_global, &prepare, edata, size_a, szind_a,
	    slab_a, trail, size_b, szind_b, slab_b);

	emap_unlock_edata2(tsdn, &emap_global, edata, trail);

	return trail;
label_error_c:
	emap_unlock_edata2(tsdn, &emap_global, edata, trail);
label_error_b:
	edata_cache_put(tsdn, edata_cache, trail);
label_error_a:
	return NULL;
}

edata_t *
extent_split_wrapper(tsdn_t *tsdn, edata_cache_t *edata_cache, ehooks_t *ehooks,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a,
    size_t size_b, szind_t szind_b, bool slab_b) {
	return extent_split_impl(tsdn, edata_cache, ehooks, edata, size_a,
	    szind_a, slab_a, size_b, szind_b, slab_b, false);
}

static bool
extent_merge_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_cache_t *edata_cache,
    edata_t *a, edata_t *b, bool growing_retained) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, growing_retained ? 1 : 0);
	assert(edata_base_get(a) < edata_base_get(b));

	assert(edata_arena_ind_get(a) == edata_arena_ind_get(b));
	assert(edata_arena_ind_get(a) == ehooks_ind_get(ehooks));

	bool err = ehooks_merge(tsdn, ehooks, edata_base_get(a),
	    edata_size_get(a), edata_is_head_get(a), edata_base_get(b),
	    edata_size_get(b), edata_is_head_get(b), edata_committed_get(a));

	if (err) {
		return true;
	}

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than extent_{,de}register() to do things in the right order.
	 */
	emap_prepare_t prepare;
	emap_merge_prepare(tsdn, &emap_global, &prepare, a, b);

	emap_lock_edata2(tsdn, &emap_global, a, b);
	emap_merge_commit(tsdn, &emap_global, &prepare, a, b);
	emap_unlock_edata2(tsdn, &emap_global, a, b);

	edata_cache_put(tsdn, edata_cache, b);

	return false;
}

bool
extent_merge_wrapper(tsdn_t *tsdn, ehooks_t *ehooks, edata_cache_t *edata_cache,
    edata_t *a, edata_t *b) {
	return extent_merge_impl(tsdn, ehooks, edata_cache, a, b, false);
}

bool
extent_boot(void) {
	assert(sizeof(slab_data_t) >= sizeof(e_prof_info_t));

	if (have_dss) {
		extent_dss_boot();
	}

	return false;
}
