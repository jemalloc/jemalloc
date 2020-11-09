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
	if (LG_SIZEOF_PTR == 2) {
		return false;
	}
	/*
	 * We use the edata bitmap; it needs to have at least as many bits as a
	 * hugepage has pages.
	 */
	if (HUGEPAGE / PAGE > BITMAP_GROUPS_MAX * sizeof(bitmap_t) * 8) {
		return false;
	}
	return true;
}

bool
hpa_shard_init(hpa_shard_t *shard, emap_t *emap, edata_cache_t *edata_cache,
    unsigned ind, size_t alloc_max) {
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
	edata_cache_small_init(&shard->ecs, edata_cache);
	psset_init(&shard->psset);
	shard->alloc_max = alloc_max;
	edata_list_inactive_init(&shard->unused_slabs);
	shard->eden = NULL;
	shard->ind = ind;
	shard->emap = emap;

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
}

void
hpa_shard_stats_merge(tsdn_t *tsdn, hpa_shard_t *shard,
    hpa_shard_stats_t *dst) {
	malloc_mutex_lock(tsdn, &shard->mtx);
	psset_stats_accum(&dst->psset_stats, &shard->psset.stats);
	malloc_mutex_unlock(tsdn, &shard->mtx);
}

static bool
hpa_should_hugify(hpa_shard_t *shard, edata_t *ps) {
	/*
	 * For now, just use a static check; hugify a page if it's <= 5%
	 * inactive.  Eventually, this should be a malloc conf option.
	 */
	return !edata_hugeified_get(ps)
	    && edata_nfree_get(ps) < (HUGEPAGE / PAGE) * 5 / 100;
}

/* Returns true on error. */
static void
hpa_hugify(edata_t *ps) {
	assert(edata_size_get(ps) == HUGEPAGE);
	assert(edata_hugeified_get(ps));
	bool err = pages_huge(edata_base_get(ps), HUGEPAGE);
	/*
	 * Eat the error; even if the hugeification failed, it's still safe to
	 * pretend it didn't (and would require extraordinary measures to
	 * unhugify).
	 */
	(void)err;
}

static void
hpa_dehugify(edata_t *ps) {
	/* Purge, then dehugify while unbacked. */
	pages_purge_forced(edata_addr_get(ps), HUGEPAGE);
	pages_nohuge(edata_addr_get(ps), HUGEPAGE);
	edata_hugeified_set(ps, false);
}

static edata_t *
hpa_grow(tsdn_t *tsdn, hpa_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->grow_mtx);
	edata_t *ps = NULL;

	/* Is there address space waiting for reuse? */
	malloc_mutex_assert_owner(tsdn, &shard->grow_mtx);
	ps = edata_list_inactive_first(&shard->unused_slabs);
	if (ps != NULL) {
		edata_list_inactive_remove(&shard->unused_slabs, ps);
		return ps;
	}

	/* Is eden a perfect fit? */
	if (shard->eden != NULL && edata_size_get(shard->eden) == HUGEPAGE) {
		ps = shard->eden;
		shard->eden = NULL;
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
		malloc_mutex_lock(tsdn, &shard->mtx);
		/* Allocate ps edata, bailing if we fail. */
		ps = edata_cache_small_get(tsdn, &shard->ecs);
		if (ps == NULL) {
			malloc_mutex_unlock(tsdn, &shard->mtx);
			pages_unmap(new_eden, HPA_EDEN_SIZE);
			return NULL;
		}
		/* Allocate eden edata, bailing if we fail. */
		shard->eden = edata_cache_small_get(tsdn, &shard->ecs);
		if (shard->eden == NULL) {
			edata_cache_small_put(tsdn, &shard->ecs, ps);
			malloc_mutex_unlock(tsdn, &shard->mtx);
			pages_unmap(new_eden, HPA_EDEN_SIZE);
			return NULL;
		}
		/* Success. */
		malloc_mutex_unlock(tsdn, &shard->mtx);

		/*
		 * Note that the values here don't really make sense (e.g. eden
		 * is actually zeroed).  But we don't use the slab metadata in
		 * determining subsequent allocation metadata (e.g. zero
		 * tracking should be done at the per-page level, not at the
		 * level of the hugepage).  It's just a convenient data
		 * structure that contains much of the helpers we need (defined
		 * lists, a bitmap, an address field, etc.).  Eventually, we'll
		 * have a "real" representation of a hugepage that's unconnected
		 * to the edata_ts it will serve allocations into.
		 */
		edata_init(shard->eden, shard->ind, new_eden, HPA_EDEN_SIZE,
		    /* slab */ false, SC_NSIZES, /* sn */ 0, extent_state_dirty,
		    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
		    /* is_head */ true);
		edata_hugeified_set(shard->eden, false);
	} else {
		/* Eden is already nonempty; only need an edata for ps. */
		malloc_mutex_lock(tsdn, &shard->mtx);
		ps = edata_cache_small_get(tsdn, &shard->ecs);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		if (ps == NULL) {
			return NULL;
		}
	}
	/*
	 * We should have dropped mtx since we're not touching ecs any more, but
	 * we should continue to hold the grow mutex, since we're about to touch
	 * eden.
	 */
	malloc_mutex_assert_not_owner(tsdn, &shard->mtx);
	malloc_mutex_assert_owner(tsdn, &shard->grow_mtx);

	assert(shard->eden != NULL);
	assert(edata_size_get(shard->eden) > HUGEPAGE);
	assert(edata_size_get(shard->eden) % HUGEPAGE == 0);
	assert(edata_addr_get(shard->eden)
	    == HUGEPAGE_ADDR2BASE(edata_addr_get(shard->eden)));
	malloc_mutex_lock(tsdn, &shard->mtx);
	ps = edata_cache_small_get(tsdn, &shard->ecs);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (ps == NULL) {
		return NULL;
	}
	edata_init(ps, edata_arena_ind_get(shard->eden),
	    edata_addr_get(shard->eden), HUGEPAGE, /* slab */ false,
	    /* szind */ SC_NSIZES, /* sn */ 0, extent_state_dirty,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    /* is_head */ true);
	edata_hugeified_set(ps, false);
	edata_addr_set(shard->eden, edata_past_get(ps));
	edata_size_set(shard->eden,
	    edata_size_get(shard->eden) - HUGEPAGE);

	return ps;
}

/*
 * The psset does not hold empty slabs.  Upon becoming empty, then, we need to
 * put them somewhere.  We take this as an opportunity to purge, and retain
 * their address space in a list outside the psset.
 */
static void
hpa_handle_ps_eviction(tsdn_t *tsdn, hpa_shard_t *shard, edata_t *ps) {
	/*
	 * We do relatively expensive system calls.  The ps was evicted, so no
	 * one should touch it while we're also touching it.
	 */
	malloc_mutex_assert_not_owner(tsdn, &shard->mtx);
	malloc_mutex_assert_not_owner(tsdn, &shard->grow_mtx);

	assert(edata_size_get(ps) == HUGEPAGE);
	assert(HUGEPAGE_ADDR2BASE(edata_addr_get(ps)) == edata_addr_get(ps));

	/*
	 * We do this unconditionally, even for pages which were not originally
	 * hugeified; it has the same effect.
	 */
	hpa_dehugify(ps);

	malloc_mutex_lock(tsdn, &shard->grow_mtx);
	edata_list_inactive_prepend(&shard->unused_slabs, ps);
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

	err = psset_alloc_reuse(&shard->psset, edata, size);
	if (err) {
		edata_cache_small_put(tsdn, &shard->ecs, edata);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		return NULL;
	}
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
		edata_t *ps = psset_dalloc(&shard->psset, edata);
		/*
		 * The pageslab was nonempty before we started; it
		 * should still be nonempty now, and so shouldn't get
		 * evicted.
		 */
		assert(ps == NULL);
		edata_cache_small_put(tsdn, &shard->ecs, edata);
		malloc_mutex_unlock(tsdn, &shard->mtx);
		*oom = true;
		return NULL;
	}

	edata_t *ps = edata_ps_get(edata);
	assert(ps != NULL);
	bool hugify = hpa_should_hugify(shard, ps);
	if (hugify) {
		/*
		 * Do the metadata modification while holding the lock; we'll
		 * actually change state with the lock dropped.
		 */
		psset_hugify(&shard->psset, ps);
	}
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
		hpa_hugify(ps);
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
	edata_t *grow_edata = hpa_grow(tsdn, shard);
	if (grow_edata == NULL) {
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return NULL;
	}
	assert(edata_arena_ind_get(grow_edata) == shard->ind);

	edata_slab_set(grow_edata, true);
	fb_group_t *fb = edata_slab_data_get(grow_edata)->bitmap;
	fb_init(fb, HUGEPAGE / PAGE);

	/* We got the new edata; allocate from it. */
	malloc_mutex_lock(tsdn, &shard->mtx);
	edata = edata_cache_small_get(tsdn, &shard->ecs);
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &shard->mtx);
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		return NULL;
	}
	psset_alloc_new(&shard->psset, grow_edata, edata, size);
	err = emap_register_boundary(tsdn, shard->emap, edata,
	    SC_NSIZES, /* slab */ false);
	if (err) {
		edata_t *ps = psset_dalloc(&shard->psset, edata);
		/*
		 * The pageslab was empty except for the new allocation; it
		 * should get evicted.
		 */
		assert(ps == grow_edata);
		edata_cache_small_put(tsdn, &shard->ecs, edata);
		/*
		 * Technically the same as fallthrough at the time of this
		 * writing, but consistent with the error handling in the rest
		 * of the function.
		 */
		malloc_mutex_unlock(tsdn, &shard->mtx);
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);
		hpa_handle_ps_eviction(tsdn, shard, ps);
		return NULL;
	}
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

	edata_t *ps = edata_ps_get(edata);
	/* Currently, all edatas come from pageslabs. */
	assert(ps != NULL);
	emap_deregister_boundary(tsdn, shard->emap, edata);
	malloc_mutex_lock(tsdn, &shard->mtx);
	/*
	 * Note that the shard mutex protects the edata hugeified field, too.
	 * Page slabs can move between pssets (and have their hugeified status
	 * change) in racy ways.
	 */
	edata_t *evicted_ps = psset_dalloc(&shard->psset, edata);
	/*
	 * If a pageslab became empty because of the dalloc, it better have been
	 * the one we expected.
	 */
	assert(evicted_ps == NULL || evicted_ps == ps);
	edata_cache_small_put(tsdn, &shard->ecs, edata);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (evicted_ps != NULL) {
		hpa_handle_ps_eviction(tsdn, shard, evicted_ps);
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
	edata_t edata = {0};
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	bool psset_empty = psset_alloc_reuse(psset, &edata, PAGE);
	assert(psset_empty);
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
	edata_t *ps;
	while ((ps = edata_list_inactive_first(&shard->unused_slabs)) != NULL) {
		assert(edata_size_get(ps) == HUGEPAGE);
		edata_list_inactive_remove(&shard->unused_slabs, ps);
		pages_unmap(edata_base_get(ps), HUGEPAGE);
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
