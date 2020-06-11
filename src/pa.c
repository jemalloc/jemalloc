#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

static edata_t *ecache_pai_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero);
static bool ecache_pai_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero);
static bool ecache_pai_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size);
static void ecache_pai_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata);

static void
pa_nactive_add(pa_shard_t *shard, size_t add_pages) {
	atomic_fetch_add_zu(&shard->nactive, add_pages, ATOMIC_RELAXED);
}

static void
pa_nactive_sub(pa_shard_t *shard, size_t sub_pages) {
	assert(atomic_load_zu(&shard->nactive, ATOMIC_RELAXED) >= sub_pages);
	atomic_fetch_sub_zu(&shard->nactive, sub_pages, ATOMIC_RELAXED);
}

bool
pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, emap_t *emap, base_t *base,
    unsigned ind, pa_shard_stats_t *stats, malloc_mutex_t *stats_mtx,
    nstime_t *cur_time, ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms) {
	/* This will change eventually, but for now it should hold. */
	assert(base_ind_get(base) == ind);
	if (edata_cache_init(&shard->edata_cache, base)) {
		return true;
	}
	if (pac_init(tsdn, &shard->pac, base, emap, &shard->edata_cache,
	    cur_time, dirty_decay_ms, muzzy_decay_ms, &stats->pac_stats,
	    stats_mtx)) {
		return true;
	}

	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);

	shard->stats_mtx = stats_mtx;
	shard->stats = stats;
	memset(shard->stats, 0, sizeof(*shard->stats));

	shard->emap = emap;
	shard->base = base;

	shard->ecache_pai.alloc = &ecache_pai_alloc;
	shard->ecache_pai.expand = &ecache_pai_expand;
	shard->ecache_pai.shrink = &ecache_pai_shrink;
	shard->ecache_pai.dalloc = &ecache_pai_dalloc;

	return false;
}

void
pa_shard_reset(pa_shard_t *shard) {
	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);
}

void
pa_shard_destroy(tsdn_t *tsdn, pa_shard_t *shard) {
	pac_destroy(tsdn, &shard->pac);
}

static inline bool
pa_shard_may_have_muzzy(pa_shard_t *shard) {
	return pac_decay_ms_get(&shard->pac, extent_state_muzzy) != 0;
}

static edata_t *
ecache_pai_alloc(tsdn_t *tsdn, pai_t *self, size_t size, size_t alignment,
    bool zero) {
	pa_shard_t *shard =
	    (pa_shard_t *)((uintptr_t)self - offsetof(pa_shard_t, ecache_pai));

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	edata_t *edata = ecache_alloc(tsdn, &shard->pac, ehooks,
	    &shard->pac.ecache_dirty, NULL, size, alignment, zero);

	if (edata == NULL && pa_shard_may_have_muzzy(shard)) {
		edata = ecache_alloc(tsdn, &shard->pac, ehooks,
		    &shard->pac.ecache_muzzy, NULL, size, alignment, zero);
	}
	if (edata == NULL) {
		edata = ecache_alloc_grow(tsdn, &shard->pac, ehooks,
		    &shard->pac.ecache_retained, NULL, size, alignment, zero);
		if (config_stats && edata != NULL) {
			atomic_fetch_add_zu(&shard->pac.stats->pac_mapped, size,
			    ATOMIC_RELAXED);
		}
	}
	return edata;
}

edata_t *
pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size, size_t alignment,
    bool slab, szind_t szind, bool zero) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);

	edata_t *edata = pai_alloc(tsdn, &shard->ecache_pai, size, alignment,
	    zero);

	if (edata != NULL) {
		pa_nactive_add(shard, size >> LG_PAGE);
		emap_remap(tsdn, shard->emap, edata, szind, slab);
		edata_szind_set(edata, szind);
		edata_slab_set(edata, slab);
		if (slab) {
			emap_register_interior(tsdn, shard->emap, edata, szind);
		}
	}
	return edata;
}

static bool
ecache_pai_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size, bool zero) {
	pa_shard_t *shard =
	    (pa_shard_t *)((uintptr_t)self - offsetof(pa_shard_t, ecache_pai));

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	void *trail_begin = edata_past_get(edata);

	size_t mapped_add = 0;
	size_t expand_amount = new_size - old_size;

	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}
	edata_t *trail = ecache_alloc(tsdn, &shard->pac, ehooks,
	    &shard->pac.ecache_dirty, trail_begin, expand_amount, PAGE, zero);
	if (trail == NULL) {
		trail = ecache_alloc(tsdn, &shard->pac, ehooks,
		    &shard->pac.ecache_muzzy, trail_begin, expand_amount, PAGE,
		    zero);
	}
	if (trail == NULL) {
		trail = ecache_alloc_grow(tsdn, &shard->pac, ehooks,
		    &shard->pac.ecache_retained, trail_begin, expand_amount,
		    PAGE, zero);
		mapped_add = expand_amount;
	}
	if (trail == NULL) {
		return true;
	}
	if (extent_merge_wrapper(tsdn, &shard->pac, ehooks, edata, trail)) {
		extent_dalloc_wrapper(tsdn, &shard->pac, ehooks, trail);
		return true;
	}
	if (config_stats && mapped_add > 0) {
		atomic_fetch_add_zu(&shard->pac.stats->pac_mapped, mapped_add,
		    ATOMIC_RELAXED);
	}
	return false;
}

bool
pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool zero) {
	assert(new_size > old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);

	size_t expand_amount = new_size - old_size;

	bool error = pai_expand(tsdn, &shard->ecache_pai, edata, old_size,
	    new_size, zero);
	if (error) {
		return true;
	}

	pa_nactive_add(shard, expand_amount >> LG_PAGE);
	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

static bool
ecache_pai_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size) {
	pa_shard_t *shard =
	    (pa_shard_t *)((uintptr_t)self - offsetof(pa_shard_t, ecache_pai));

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	size_t shrink_amount = old_size - new_size;


	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	edata_t *trail = extent_split_wrapper(tsdn, &shard->pac, ehooks, edata,
	    new_size, shrink_amount);
	if (trail == NULL) {
		return true;
	}
	ecache_dalloc(tsdn, &shard->pac, ehooks, &shard->pac.ecache_dirty,
	    trail);
	return false;
}

bool
pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool *generated_dirty) {
	assert(new_size < old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);
	size_t shrink_amount = old_size - new_size;

	*generated_dirty = false;
	bool error = pai_shrink(tsdn, &shard->ecache_pai, edata, old_size,
	    new_size);
	if (error) {
		return true;
	}
	pa_nactive_sub(shard, shrink_amount >> LG_PAGE);
	*generated_dirty = true;

	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

static void
ecache_pai_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata) {
	pa_shard_t *shard =
	    (pa_shard_t *)((uintptr_t)self - offsetof(pa_shard_t, ecache_pai));
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	ecache_dalloc(tsdn, &shard->pac, ehooks, &shard->pac.ecache_dirty,
	    edata);
}

void
pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *generated_dirty) {
	emap_remap(tsdn, shard->emap, edata, SC_NSIZES, /* slab */ false);
	if (edata_slab_get(edata)) {
		emap_deregister_interior(tsdn, shard->emap, edata);
		edata_slab_set(edata, false);
	}
	edata_szind_set(edata, SC_NSIZES);
	pa_nactive_sub(shard, edata_size_get(edata) >> LG_PAGE);
	pai_dalloc(tsdn, &shard->ecache_pai, edata);
	*generated_dirty = true;
}

static size_t
pa_stash_decayed(tsdn_t *tsdn, pa_shard_t *shard, ecache_t *ecache,
    size_t npages_limit, size_t npages_decay_max,
    edata_list_inactive_t *result) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	while (nstashed < npages_decay_max) {
		edata_t *edata = ecache_evict(tsdn, &shard->pac, ehooks, ecache,
		    npages_limit);
		if (edata == NULL) {
			break;
		}
		edata_list_inactive_append(result, edata);
		nstashed += edata_size_get(edata) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
pa_decay_stashed(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    edata_list_inactive_t *decay_extents) {
	bool err;

	size_t nmadvise = 0;
	size_t nunmapped = 0;
	size_t npurged = 0;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	bool try_muzzy = !fully_decay && pa_shard_may_have_muzzy(shard);

	for (edata_t *edata = edata_list_inactive_first(decay_extents);
	    edata != NULL; edata = edata_list_inactive_first(decay_extents)) {
		edata_list_inactive_remove(decay_extents, edata);

		size_t size = edata_size_get(edata);
		size_t npages = size >> LG_PAGE;

		nmadvise++;
		npurged += npages;

		switch (ecache->state) {
		case extent_state_active:
			not_reached();
		case extent_state_dirty:
			if (try_muzzy) {
				err = extent_purge_lazy_wrapper(tsdn, ehooks,
				    edata, /* offset */ 0, size);
				if (!err) {
					ecache_dalloc(tsdn, &shard->pac, ehooks,
					    &shard->pac.ecache_muzzy, edata);
					break;
				}
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, &shard->pac, ehooks, edata);
			nunmapped += npages;
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, *shard->stats_mtx);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->npurge, 1);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->nmadvise, nmadvise);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &decay_stats->purged, npurged);
		LOCKEDINT_MTX_UNLOCK(tsdn, *shard->stats_mtx);
		atomic_fetch_sub_zu(&shard->pac.stats->pac_mapped,
		    nunmapped << LG_PAGE, ATOMIC_RELAXED);
	}

	return npurged;
}

/*
 * npages_limit: Decay at most npages_decay_max pages without violating the
 * invariant: (ecache_npages_get(ecache) >= npages_limit).  We need an upper
 * bound on number of pages in order to prevent unbounded growth (namely in
 * stashed), otherwise unbounded new pages could be added to extents during the
 * current decay run, so that the purging thread never finishes.
 */
static void
pa_decay_to_limit(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    size_t npages_limit, size_t npages_decay_max) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 1);

	if (decay->purging || npages_decay_max == 0) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	edata_list_inactive_t decay_extents;
	edata_list_inactive_init(&decay_extents);
	size_t npurge = pa_stash_decayed(tsdn, shard, ecache, npages_limit,
	    npages_decay_max, &decay_extents);
	if (npurge != 0) {
		size_t npurged = pa_decay_stashed(tsdn, shard, decay,
		    decay_stats, ecache, fully_decay, &decay_extents);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

void
pa_decay_all(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);
	pa_decay_to_limit(tsdn, shard, decay, decay_stats, ecache, fully_decay,
	    /* npages_limit */ 0, ecache_npages_get(ecache));
}

bool
pa_shard_retain_grow_limit_get_set(tsdn_t *tsdn, pa_shard_t *shard,
    size_t *old_limit, size_t *new_limit) {
	return pac_retain_grow_limit_get_set(tsdn, &shard->pac, old_limit,
	    new_limit);
}

bool
pa_decay_ms_set(tsdn_t *tsdn, pa_shard_t *shard, extent_state_t state,
    ssize_t decay_ms, pac_purge_eagerness_t eagerness) {
	return pac_decay_ms_set(tsdn, &shard->pac, state, decay_ms, eagerness);
}

ssize_t
pa_decay_ms_get(pa_shard_t *shard, extent_state_t state) {
	return pac_decay_ms_get(&shard->pac, state);
}
