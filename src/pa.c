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
pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t new_usize,
    szind_t szind, bool slab, bool *zero, size_t *mapped_add) {
	ehooks_t *ehooks = pa_shard_ehooks_get(shard);
	size_t old_usize = edata_usize_get(edata);
	size_t trail_size = new_usize - old_usize;
	void *trail_begin = edata_past_get(edata);

	*mapped_add = 0;
	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}
	edata_t *trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_dirty,
	    trail_begin, trail_size, PAGE, /* slab */ false, SC_NSIZES, zero);
	if (trail == NULL) {
		trail = ecache_alloc(tsdn, shard, ehooks, &shard->ecache_muzzy,
		    trail_begin, trail_size, PAGE, /* slab */ false, SC_NSIZES,
		    zero);
	}
	if (trail == NULL) {
		trail = ecache_alloc_grow(tsdn, shard, ehooks,
		    &shard->ecache_retained, trail_begin, trail_size, PAGE,
		    /* slab */ false, SC_NSIZES, zero);
		*mapped_add = trail_size;
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
