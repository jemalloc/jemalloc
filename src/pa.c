#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, base_t *base, unsigned ind,
    pa_shard_stats_t *stats, malloc_mutex_t *stats_mtx) {
	/* This will change eventually, but for now it should hold. */
	assert(base_ind_get(base) == ind);
	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (ecache_init(tsdn, &shard->ecache_dirty, extent_state_dirty, ind,
	    /* delay_coalesce */ true)) {
		return true;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (ecache_init(tsdn, &shard->ecache_muzzy, extent_state_muzzy, ind,
	    /* delay_coalesce */ false)) {
		return true;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (ecache_init(tsdn, &shard->ecache_retained, extent_state_retained,
	    ind, /* delay_coalesce */ false)) {
		return true;
	}
	if (edata_cache_init(&shard->edata_cache, base)) {
		return true;
	}

	if (ecache_grow_init(tsdn, &shard->ecache_grow)) {
		return true;
	}

	atomic_store_zu(&shard->extent_sn_next, 0, ATOMIC_RELAXED);

	shard->stats_mtx = stats_mtx;
	shard->stats = stats;
	memset(shard->stats, 0, sizeof(*shard->stats));

	shard->base = base;

	return false;
}

size_t
pa_shard_extent_sn_next(pa_shard_t *shard) {
	return atomic_fetch_add_zu(&shard->extent_sn_next, 1, ATOMIC_RELAXED);
}

static bool
pa_shard_may_have_muzzy(pa_shard_t *shard) {
	return pa_shard_muzzy_decay_ms_get(shard) != 0;
}

edata_t *
pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size, size_t alignment,
    bool slab, szind_t szind, bool *zero, size_t *mapped_add) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	*mapped_add = 0;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	edata_t *edata = ecache_alloc(tsdn, shard, ehooks,
	    &shard->ecache_dirty, NULL, size, alignment, slab, szind,
	    zero);
	if (edata == NULL && pa_shard_may_have_muzzy(shard)) {
		edata = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_muzzy,
		    NULL, size, alignment, slab, szind, zero);
	}
	if (edata == NULL) {
		edata = ecache_alloc_grow(tsdn, shard, ehooks,
		    &shard->ecache_retained, NULL, size, alignment, slab,
		    szind, zero);
		if (config_stats) {
			/*
			 * edata may be NULL on OOM, but in that case mapped_add
			 * isn't used below, so there's no need to conditionlly
			 * set it to 0 here.
			 */
			*mapped_add = size;
		}
	}
	return edata;
}

bool
pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool slab, bool *zero, size_t *mapped_add) {
	assert(new_size > old_size);
	assert(edata_size_get(edata) == old_size);

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	void *trail_begin = edata_past_get(edata);
	size_t expand_amount = new_size - old_size;

	*mapped_add = 0;
	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}
	edata_t *trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_dirty,
	    trail_begin, expand_amount, PAGE, /* slab */ false, SC_NSIZES,
	    zero);
	if (trail == NULL) {
		trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_muzzy,
		    trail_begin, expand_amount, PAGE, /* slab */ false,
		    SC_NSIZES, zero);
	}
	if (trail == NULL) {
		trail = ecache_alloc_grow(tsdn, shard, ehooks,
		    &shard->ecache_retained, trail_begin, expand_amount, PAGE,
		    /* slab */ false, SC_NSIZES, zero);
		*mapped_add = expand_amount;
	}
	if (trail == NULL) {
		*mapped_add = 0;
		return true;
	}
	if (extent_merge_wrapper(tsdn, ehooks, &shard->edata_cache, edata,
	    trail)) {
		extent_dalloc_wrapper(tsdn, shard, ehooks, trail);
		*mapped_add = 0;
		return true;
	}
	emap_remap(tsdn, &emap_global, edata, szind, slab);
	return false;
}

bool
pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool slab, bool *generated_dirty) {
	assert(new_size < old_size);

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	*generated_dirty = false;

	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	edata_t *trail = extent_split_wrapper(tsdn, &shard->edata_cache, ehooks,
	    edata, new_size, szind, slab, old_size - new_size, SC_NSIZES,
	    false);
	if (trail == NULL) {
		return true;
	}
	ecache_dalloc(tsdn, shard, ehooks, &shard->ecache_dirty, trail);
	*generated_dirty = true;
	return false;
}

void
pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *generated_dirty) {
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	ecache_dalloc(tsdn, shard, ehooks, &shard->ecache_dirty, edata);
	*generated_dirty = true;
}

size_t
pa_stash_decayed(tsdn_t *tsdn, pa_shard_t *shard, ecache_t *ecache,
    size_t npages_limit, size_t npages_decay_max, edata_list_t *result) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	while (nstashed < npages_decay_max) {
		edata_t *edata = ecache_evict(tsdn, shard, ehooks, ecache,
		    npages_limit);
		if (edata == NULL) {
			break;
		}
		edata_list_append(result, edata);
		nstashed += edata_size_get(edata) >> LG_PAGE;
	}
	return nstashed;
}

size_t
pa_decay_stashed(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    edata_list_t *decay_extents) {
	bool err;

	size_t nmadvise = 0;
	size_t nunmapped = 0;
	size_t npurged = 0;

	ehooks_t *ehooks = pa_shard_ehooks_get(shard);

	bool try_muzzy = !fully_decay && pa_shard_may_have_muzzy(shard);

	for (edata_t *edata = edata_list_first(decay_extents); edata !=
	    NULL; edata = edata_list_first(decay_extents)) {
		edata_list_remove(decay_extents, edata);

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
					ecache_dalloc(tsdn, shard, ehooks,
					    &shard->ecache_muzzy, edata);
					break;
				}
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, shard, ehooks, edata);
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
		locked_dec_zu(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
		    &shard->stats->mapped, nunmapped << LG_PAGE);
		LOCKEDINT_MTX_UNLOCK(tsdn, *shard->stats_mtx);
	}

	return npurged;
}
