#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa.h"

#include "jemalloc/internal/flat_bitmap.h"
#include "jemalloc/internal/witness.h"

#define HPA_EDEN_SIZE (128 * HUGEPAGE)

static edata_t *hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero);
static bool hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero);
static bool hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size);
static void hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata);

bool
hpa_supported() {
#ifdef _WIN32
	/*
	 * At least until the API and implementation is somewhat settled, we
	 * don't want to try to debug the VM subsystem on the hardest-to-test
	 * platform.
	 */
	return false;
#endif
	if (!pages_can_hugify) {
		return false;
	}
	/*
	 * We fundamentally rely on a address-space-hungry growth strategy for
	 * hugepages.
	 */
	if (LG_SIZEOF_PTR != 3) {
		return false;
	}
	/*
	 * If we couldn't detect the value of HUGEPAGE, HUGEPAGE_PAGES becomes
	 * this sentinel value -- see the comment in pages.h.
	 */
	if (HUGEPAGE_PAGES == 1) {
		return false;
	}
	return true;
}

bool
hpa_shard_init(hpa_shard_t *shard, emap_t *emap, base_t *base,
    edata_cache_t *edata_cache, unsigned ind, size_t alloc_max) {
	/* malloc_conf processing should have filtered out these cases. */
	assert(hpa_supported());
	bool err;
	err = malloc_mutex_init(&shard->grow_mtx, "hpa_shard_grow",
	    WITNESS_RANK_HPA_SHARD_GROW, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	err = malloc_mutex_init(&shard->mtx, "hpa_shard",
	    WITNESS_RANK_HPA_SHARD, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	assert(edata_cache != NULL);
	shard->base = base;
	edata_cache_small_init(&shard->ecs, edata_cache);
	psset_init(&shard->psset);
	shard->alloc_max = alloc_max;
	hpdata_list_init(&shard->unused_slabs);
	shard->age_counter = 0;
	shard->eden = NULL;
	shard->eden_len = 0;
	shard->ind = ind;
	shard->emap = emap;
	shard->nevictions = 0;

	/*
	 * Fill these in last, so that if an hpa_shard gets used despite
	 * initialization failing, we'll at least crash instead of just
	 * operating on corrupted data.
	 */
	shard->pai.alloc = &hpa_alloc;
	shard->pai.expand = &hpa_expand;
	shard->pai.shrink = &hpa_shrink;
	shard->pai.dalloc = &hpa_dalloc;

	return false;
}

/*
 * Note that the stats functions here follow the usual stats naming conventions;
 * "merge" obtains the stats from some live object of instance, while "accum"
 * only combines the stats from one stats objet to another.  Hence the lack of
 * locking here.
 */
void
hpa_shard_stats_accum(hpa_shard_stats_t *dst, hpa_shard_stats_t *src) {
	psset_stats_accum(&dst->psset_stats, &src->psset_stats);
	dst->nevictions += src->nevictions;
}

void
hpa_shard_stats_merge(tsdn_t *tsdn, hpa_shard_t *shard,
    hpa_shard_stats_t *dst) {
	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	malloc_mutex_lock(tsdn, &shard->mtx);
	psset_stats_accum(&dst->psset_stats, &shard->psset.stats);
	dst->nevictions += shard->nevictions;
	malloc_mutex_unlock(tsdn, &shard->mtx);
	malloc_mutex_unlock(tsdn, &shard->grow_mtx);
}

static hpdata_t *
hpa_alloc_ps(tsdn_t *tsdn, hpa_shard_t *shard) {
	return (hpdata_t *)base_alloc(tsdn, shard->base, sizeof(hpdata_t),
	    CACHELINE);
}

static bool
hpa_should_hugify(hpa_shard_t *shard, hpdata_t *ps) {
	/*
	 * For now, just use a static check; hugify a page if it's <= 5%
	 * inactive.  Eventually, this should be a malloc conf option.
	 */
	if (hpdata_changing_state_get(ps)) {
		return false;
	}
	return !hpdata_huge_get(ps)
	    && hpdata_nactive_get(ps) >= (HUGEPAGE_PAGES) * 95 / 100;
}

/*
 * Whether or not the given pageslab meets the criteria for being purged (and,
 * if necessary, dehugified).
 */
static bool
hpa_should_purge(hpa_shard_t *shard, hpdata_t *ps) {
	/* Ditto. */
	if (hpdata_changing_state_get(ps)) {
		return false;
	}
	size_t purgeable = hpdata_ndirty_get(ps) - hpdata_nactive_get(ps);
	return purgeable > HUGEPAGE_PAGES * 25 / 100
	    || (purgeable > 0 && hpdata_empty(ps));
}

static hpdata_t *
hpa_grow(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->grow_mtx);
	hpdata_t *ps = NULL;

	/* Is there address space waiting for reuse? */
	malloc_mutex_assert_owner(tsdn, &shard->grow_mtx);
	ps = hpdata_list_first(&shard->unused_slabs);
	if (ps != NULL) {
		hpdata_list_remove(&shard->unused_slabs, ps);
		hpdata_age_set(ps, shard->age_counter++);
		return ps;
	}

	/* Is eden a perfect fit? */
	if (shard->eden != NULL && shard->eden_len == HUGEPAGE) {
		ps = hpa_alloc_ps(tsdn, shard);
		if (ps == NULL) {
			return NULL;
		}
		hpdata_init(ps, shard->eden, shard->age_counter++);
		shard->eden = NULL;
		shard->eden_len = 0;
		return ps;
	}

	/*
	 * We're about to try to allocate from eden by splitting.  If eden is
	 * NULL, we have to allocate it too.  Otherwise, we just have to
	 * allocate an edata_t for the new psset.
	 */
	if (shard->eden == NULL) {
		/*
		 * During development, we're primarily concerned with systems
		 * with overcommit.  Eventually, we should be more careful here.
		 */
		bool commit = true;
		/* Allocate address space, bailing if we fail. */
		void *new_eden = pages_map(NULL, HPA_EDEN_SIZE, HUGEPAGE,
		    &commit);
		if (new_eden == NULL) {
			return NULL;
		}
		ps = hpa_alloc_ps(tsdn, shard);
		if (ps == NULL) {
			pages_unmap(new_eden, HPA_EDEN_SIZE);
			return NULL;
		}
		shard->eden = new_eden;
		shard->eden_len = HPA_EDEN_SIZE;
	} else {
		/* Eden is already nonempty; only need an edata for ps. */
		ps = hpa_alloc_ps(tsdn, shard);
		if (ps == NULL) {
			return NULL;
		}
	}
	assert(ps != NULL);
	assert(shard->eden != NULL);
	assert(shard->eden_len > HUGEPAGE);
	assert(shard->eden_len % HUGEPAGE == 0);
	assert(HUGEPAGE_ADDR2BASE(shard->eden) == shard->eden);

	hpdata_init(ps, shard->eden, shard->age_counter++);

	char *eden_char = (char *)shard->eden;
	eden_char += HUGEPAGE;
	shard->eden = (void *)eden_char;
	shard->eden_len -= HUGEPAGE;

	return ps;
}

/*
 * As a precondition, ps should not be in the psset (we can handle deallocation
 * races, but not allocation ones), and we should hold the shard mutex.
 */
static void
hpa_purge(tsdn_t *tsdn, hpa_shard_t *shard, hpdata_t *ps) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	while (hpa_should_purge(shard, ps)) {
		/* Do the metadata update bit while holding the lock. */
		hpdata_purge_state_t purge_state;
		hpdata_purge_begin(ps, &purge_state);

		/*
		 * Dehugifying can only happen on the first loop iteration,
		 * since no other threads can allocate out of this ps while
		 * we're purging (and thus, can't hugify it), but there's not a
		 * natural way to express that in the control flow.
		 */
		bool needs_dehugify = false;
		if (hpdata_huge_get(ps)) {
			needs_dehugify = true;
			hpdata_dehugify(ps);
		}

		/* Drop the lock to do the OS calls. */
		malloc_mutex_unlock(tsdn, &shard->mtx);

		if (needs_dehugify) {
			pages_nohuge(hpdata_addr_get(ps), HUGEPAGE);
		}

		size_t total_purged = 0;
		void *purge_addr;
		size_t purge_size;
		while (hpdata_purge_next(ps, &purge_state, &purge_addr,
		    &purge_size)) {
			pages_purge_forced(purge_addr, purge_size);
			total_purged += purge_size;
		}

		/* Reacquire to finish our metadata update. */
		malloc_mutex_lock(tsdn, &shard->mtx);
		hpdata_purge_end(ps, &purge_state);

		assert(total_purged <= HUGEPAGE);

		/*
		 * We're not done here; other threads can't allocate out of ps
		 * while purging, but they can still deallocate.  Those
		 * deallocations could have meant more purging than what we
		 * planned ought to happen.  We have to re-check now that we've
		 * reacquired the mutex again.
		 */
	}
}

/*
 * Does the metadata tracking associated with a page slab becoming empty.  The
 * psset doesn't hold empty pageslabs, but we do want address space reuse, so we
 * track these pages outside the psset.
 */
static void
hpa_handle_ps_eviction(tsdn_t *tsdn, hpa_shard_t *shard, hpdata_t *ps) {
	/*
	 * We do relatively expensive system calls.  The ps was evicted, so no
	 * one should touch it while we're also touching it.
	 */
	malloc_mutex_assert_not_owner(tsdn, &shard->mtx);
	malloc_mutex_assert_not_owner(tsdn, &shard->grow_mtx);

	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	shard->nevictions++;
	hpdata_list_prepend(&shard->unused_slabs, ps);
	malloc_mutex_unlock(tsdn, &shard->grow_mtx);
}

static edata_t *
hpa_try_alloc_no_grow(tsdn_t *tsdn, hpa_shard_t *shard, size_t size, bool *oom) {
	bool err;
	malloc_mutex_lock(tsdn, &shard->mtx);
	edata_t *edata = edata_cache_small_get(tsdn, &shard->ecs);
	*oom = false;
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &shard->mtx);
		*oom = true;
		return NULL;
	}
	assert(edata_arena_ind_get(edata) == shard->ind);

	hpdata_t *ps = psset_fit(&shard->psset, size);
	if (ps == NULL) {
		edata_cache_small_put(tsdn, &shard->ecs, edata);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		return NULL;
	}

	psset_remove(&shard->psset, ps);
	void *addr = hpdata_reserve_alloc(ps, size);
	edata_init(edata, shard->ind, addr, size, /* slab */ false,
	    SC_NSIZES, /* sn */ 0, extent_state_active, /* zeroed */ false,
	    /* committed */ true, EXTENT_PAI_HPA, EXTENT_NOT_HEAD);
	edata_ps_set(edata, ps);

	/*
	 * This could theoretically be moved outside of the critical section,
	 * but that introduces the potential for a race.  Without the lock, the
	 * (initially nonempty, since this is the reuse pathway) pageslab we
	 * allocated out of could become otherwise empty while the lock is
	 * dropped.  This would force us to deal with a pageslab eviction down
	 * the error pathway, which is a pain.
	 */
	err = emap_register_boundary(tsdn, shard->emap, edata,
	    SC_NSIZES, /* slab */ false);
	if (err) {
		hpdata_unreserve(ps, edata_addr_get(edata),
		    edata_size_get(edata));
		/*
		 * We should arguably reset dirty state here, but this would
		 * require some sort of prepare + commit functionality that's a
		 * little much to deal with for now.
		 */
		psset_insert(&shard->psset, ps);
		edata_cache_small_put(tsdn, &shard->ecs, edata);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		*oom = true;
		return NULL;
	}

	bool hugify = hpa_should_hugify(shard, ps);
	if (hugify) {
		hpdata_hugify_begin(ps);
	}
	psset_insert(&shard->psset, ps);

	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (hugify) {
		/*
		 * Hugifying with the lock dropped is safe, even with
		 * concurrent modifications to the ps.  This relies on
		 * the fact that the current implementation will never
		 * dehugify a non-empty pageslab, and ps will never
		 * become empty before we return edata to the user to be
		 * freed.
		 *
		 * Note that holding the lock would prevent not just operations
		 * on this page slab, but also operations any other alloc/dalloc
		 * operations in this hpa shard.
		 */
		bool err = pages_huge(hpdata_addr_get(ps), HUGEPAGE);
		/*
		 * Pretending we succeed when we actually failed is safe; trying
		 * to rolllback would be tricky, though.  Eat the error.
		 */
		(void)err;

		malloc_mutex_lock(tsdn, &shard->mtx);
		hpdata_hugify_end(ps);
		if (hpa_should_purge(shard, ps)) {
			/*
			 * There was a race in which the ps went from being
			 * almost full to having lots of free space while we
			 * hugified.  Undo our operation, taking care to meet
			 * the precondition that the ps isn't in the psset.
			 */
			psset_remove(&shard->psset, ps);
			hpa_purge(tsdn, shard, ps);
			psset_insert(&shard->psset, ps);
		}
		malloc_mutex_unlock(tsdn, &shard->mtx);
	}
	return edata;
}

static edata_t *
hpa_alloc_psset(tsdn_t *tsdn, hpa_shard_t *shard, size_t size) {
	assert(size <= shard->alloc_max);
	bool err;
	bool oom;
	edata_t *edata;

	edata = hpa_try_alloc_no_grow(tsdn, shard, size, &oom);
	if (edata != NULL) {
		return edata;
	}

	/* Nothing in the psset works; we have to grow it. */
	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	/*
	 * Check for grow races; maybe some earlier thread expanded the psset
	 * in between when we dropped the main mutex and grabbed the grow mutex.
	 */
	edata = hpa_try_alloc_no_grow(tsdn, shard, size, &oom);
	if (edata != NULL || oom) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return edata;
	}

	/*
	 * Note that we don't hold shard->mtx here (while growing);
	 * deallocations (and allocations of smaller sizes) may still succeed
	 * while we're doing this potentially expensive system call.
	 */
	hpdata_t *ps = hpa_grow(tsdn, shard);
	if (ps == NULL) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return NULL;
	}

	/* We got the new edata; allocate from it. */
	malloc_mutex_lock(tsdn, &shard->mtx);
	edata = edata_cache_small_get(tsdn, &shard->ecs);
	if (edata == NULL) {
		shard->nevictions++;
		malloc_mutex_unlock(tsdn, &shard->mtx);
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		hpa_handle_ps_eviction(tsdn, shard, ps);
		return NULL;
	}

	void *addr = hpdata_reserve_alloc(ps, size);
	edata_init(edata, shard->ind, addr, size, /* slab */ false,
	    SC_NSIZES, /* sn */ 0, extent_state_active, /* zeroed */ false,
	    /* committed */ true, EXTENT_PAI_HPA, EXTENT_NOT_HEAD);
	edata_ps_set(edata, ps);

	err = emap_register_boundary(tsdn, shard->emap, edata,
	    SC_NSIZES, /* slab */ false);
	if (err) {
		hpdata_unreserve(ps, edata_addr_get(edata),
		    edata_size_get(edata));

		edata_cache_small_put(tsdn, &shard->ecs, edata);

		shard->nevictions++;
		malloc_mutex_unlock(tsdn, &shard->mtx);
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);

		/* We'll do a fake purge; the pages weren't really touched. */
		hpdata_purge_state_t purge_state;
		void *purge_addr;
		size_t purge_size;
		hpdata_purge_begin(ps, &purge_state);
		bool found_extent = hpdata_purge_next(ps, &purge_state,
		    &purge_addr, &purge_size);
		assert(found_extent);
		assert(purge_addr == addr);
		assert(purge_size == size);
		found_extent = hpdata_purge_next(ps, &purge_state,
		    &purge_addr, &purge_size);
		assert(!found_extent);
		hpdata_purge_end(ps, &purge_state);

		hpa_handle_ps_eviction(tsdn, shard, ps);
		return NULL;
	}
	psset_insert(&shard->psset, ps);

	malloc_mutex_unlock(tsdn, &shard->mtx);
	malloc_mutex_unlock(tsdn, &shard->grow_mtx);
	return edata;
}

static hpa_shard_t *
hpa_from_pai(pai_t *self) {
	assert(self->alloc = &hpa_alloc);
	assert(self->expand = &hpa_expand);
	assert(self->shrink = &hpa_shrink);
	assert(self->dalloc = &hpa_dalloc);
	return (hpa_shard_t *)self;
}

static edata_t *
hpa_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero) {
	assert((size & PAGE_MASK) == 0);
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	hpa_shard_t *shard = hpa_from_pai(self);
	/* We don't handle alignment or zeroing for now. */
	if (alignment > PAGE || zero) {
		return NULL;
	}
	if (size > shard->alloc_max) {
		return NULL;
	}

	edata_t *edata = hpa_alloc_psset(tsdn, shard, size);

	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	if (edata != NULL) {
		emap_assert_mapped(tsdn, shard->emap, edata);
		assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
		assert(edata_state_get(edata) == extent_state_active);
		assert(edata_arena_ind_get(edata) == shard->ind);
		assert(edata_szind_get_maybe_invalid(edata) == SC_NSIZES);
		assert(!edata_slab_get(edata));
		assert(edata_committed_get(edata));
		assert(edata_base_get(edata) == edata_addr_get(edata));
		assert(edata_base_get(edata) != NULL);
	}
	return edata;
}

static bool
hpa_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero) {
	/* Expand not yet supported. */
	return true;
}

static bool
hpa_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size) {
	/* Shrink not yet supported. */
	return true;
}

static void
hpa_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata) {
	hpa_shard_t *shard = hpa_from_pai(self);

	edata_addr_set(edata, edata_base_get(edata));
	edata_zeroed_set(edata, false);

	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(edata_state_get(edata) == extent_state_active);
	assert(edata_arena_ind_get(edata) == shard->ind);
	assert(edata_szind_get_maybe_invalid(edata) == SC_NSIZES);
	assert(!edata_slab_get(edata));
	assert(edata_committed_get(edata));
	assert(edata_base_get(edata) != NULL);

	hpdata_t *ps = edata_ps_get(edata);
	/* Currently, all edatas come from pageslabs. */
	assert(ps != NULL);
	emap_deregister_boundary(tsdn, shard->emap, edata);
	/*
	 * Note that the shard mutex protects ps's metadata too; it wouldn't be
	 * correct to try to read most information out of it without the lock.
	 */
	malloc_mutex_lock(tsdn, &shard->mtx);

	/*
	 * Release the metadata early, to avoid having to remember to do it
	 * while we're also doing tricky purging logic.
	 */
	void *unreserve_addr = edata_addr_get(edata);
	size_t unreserve_size = edata_size_get(edata);
	edata_cache_small_put(tsdn, &shard->ecs, edata);

	/*
	 * We have three rules interacting here:
	 * - You can't update ps metadata while it's still in the psset.  We
	 *   enforce this because it's necessary for stats tracking and metadata
	 *   management.
	 * - The ps must not be in the psset while purging.  This is because we
	 *   can't handle purge/alloc races.
	 * - Whoever removes the ps from the psset is the one to reinsert it (or
	 *   to pass it to hpa_handle_ps_eviction upon emptying).  This keeps
	 *   responsibility tracking simple.
	 */
	if (hpdata_mid_purge_get(ps)) {
		/*
		 * Another thread started purging, and so the ps is not in the
		 * psset and we can do our metadata update.  The other thread is
		 * in charge of reinserting the ps, so we're done.
		 */
		assert(!hpdata_in_psset_get(ps));
		hpdata_unreserve(ps, unreserve_addr, unreserve_size);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		return;
	}
	/*
	 * No other thread is purging, and the ps is non-empty, so it should be
	 * in the psset.
	 */
	assert(hpdata_in_psset_get(ps));
	psset_remove(&shard->psset, ps);
	hpdata_unreserve(ps, unreserve_addr, unreserve_size);
	if (!hpa_should_purge(shard, ps)) {
		/*
		 * This should be the common case; no other thread is purging,
		 * and we won't purge either.
		 */
		psset_insert(&shard->psset, ps);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		return;
	}

	/* It's our job to purge. */
	hpa_purge(tsdn, shard, ps);

	/*
	 * OK, the hpdata is as purged as we want it to be, and it's going back
	 * into the psset (if nonempty) or getting evicted (if empty).
	 */
	if (hpdata_empty(ps)) {
		malloc_mutex_unlock(tsdn, &shard->mtx);
		hpa_handle_ps_eviction(tsdn, shard, ps);
	} else {
		psset_insert(&shard->psset, ps);
		malloc_mutex_unlock(tsdn, &shard->mtx);
	}
}

void
hpa_shard_disable(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_lock(tsdn, &shard->mtx);
	edata_cache_small_disable(tsdn, &shard->ecs);
	malloc_mutex_unlock(tsdn, &shard->mtx);
}

static void
hpa_shard_assert_stats_empty(psset_bin_stats_t *bin_stats) {
	assert(bin_stats->npageslabs_huge == 0);
	assert(bin_stats->nactive_huge == 0);
	assert(bin_stats->ninactive_huge == 0);
	assert(bin_stats->npageslabs_nonhuge == 0);
	assert(bin_stats->nactive_nonhuge == 0);
	assert(bin_stats->ninactive_nonhuge == 0);
}

static void
hpa_assert_empty(tsdn_t *tsdn, hpa_shard_t *shard, psset_t *psset) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	hpdata_t *ps = psset_fit(psset, PAGE);
	assert(ps == NULL);
	hpa_shard_assert_stats_empty(&psset->stats.full_slabs);
	for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
		hpa_shard_assert_stats_empty(
		    &psset->stats.nonfull_slabs[i]);
	}
}

void
hpa_shard_destroy(tsdn_t *tsdn, hpa_shard_t *shard) {
	/*
	 * By the time we're here, the arena code should have dalloc'd all the
	 * active extents, which means we should have eventually evicted
	 * everything from the psset, so it shouldn't be able to serve even a
	 * 1-page allocation.
	 */
	if (config_debug) {
		malloc_mutex_lock(tsdn, &shard->mtx);
		hpa_assert_empty(tsdn, shard, &shard->psset);
		malloc_mutex_unlock(tsdn, &shard->mtx);
	}
	hpdata_t *ps;
	while ((ps = hpdata_list_first(&shard->unused_slabs)) != NULL) {
		hpdata_list_remove(&shard->unused_slabs, ps);
		pages_unmap(hpdata_addr_get(ps), HUGEPAGE);
	}
}

void
hpa_shard_prefork3(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->grow_mtx);
}

void
hpa_shard_prefork4(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_parent(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_postfork_parent(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_parent(tsdn, &shard->mtx);
}

void
hpa_shard_postfork_child(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_postfork_child(tsdn, &shard->grow_mtx);
	malloc_mutex_postfork_child(tsdn, &shard->mtx);
}
