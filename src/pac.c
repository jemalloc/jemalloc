#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/pac.h"

bool
pac_init(tsdn_t *tsdn, pac_t *pac, unsigned ind, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms, pac_stats_t *pac_stats, malloc_mutex_t *stats_mtx) {
	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (ecache_init(tsdn, &pac->ecache_dirty, extent_state_dirty, ind,
	    /* delay_coalesce */ true)) {
		return true;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (ecache_init(tsdn, &pac->ecache_muzzy, extent_state_muzzy, ind,
	    /* delay_coalesce */ false)) {
		return true;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (ecache_init(tsdn, &pac->ecache_retained, extent_state_retained,
	    ind, /* delay_coalesce */ false)) {
		return true;
	}
	if (ecache_grow_init(tsdn, &pac->ecache_grow)) {
		return true;
	}
	if (decay_init(&pac->decay_dirty, cur_time, dirty_decay_ms)) {
		return true;
	}
	if (decay_init(&pac->decay_muzzy, cur_time, muzzy_decay_ms)) {
		return true;
	}

	pac->emap = emap;
	pac->edata_cache = edata_cache;
	pac->stats = pac_stats;
	pac->stats_mtx = stats_mtx;
	return false;
}

bool
pac_retain_grow_limit_get_set(tsdn_t *tsdn, pac_t *pac, size_t *old_limit,
    size_t *new_limit) {
	pszind_t new_ind JEMALLOC_CC_SILENCE_INIT(0);
	if (new_limit != NULL) {
		size_t limit = *new_limit;
		/* Grow no more than the new limit. */
		if ((new_ind = sz_psz2ind(limit + 1) - 1) >= SC_NPSIZES) {
			return true;
		}
	}

	malloc_mutex_lock(tsdn, &pac->ecache_grow.mtx);
	if (old_limit != NULL) {
		*old_limit = sz_pind2sz(pac->ecache_grow.limit);
	}
	if (new_limit != NULL) {
		pac->ecache_grow.limit = new_ind;
	}
	malloc_mutex_unlock(tsdn, &pac->ecache_grow.mtx);

	return false;
}

