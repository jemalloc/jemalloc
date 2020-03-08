#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, base_t *base, unsigned ind,
    pa_shard_stats_t *stats) {
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

	shard->stats = stats;
	memset(shard->stats, 0, sizeof(*shard->stats));

	return false;
}
