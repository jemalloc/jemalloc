#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/pac.h"

static ehooks_t *
pac_ehooks_get(pac_t *pac) {
	return base_ehooks_get(pac->base);
}

static inline void
pac_decay_data_get(pac_t *pac, extent_state_t state,
    decay_t **r_decay, pac_decay_stats_t **r_decay_stats, ecache_t **r_ecache) {
	switch(state) {
	case extent_state_dirty:
		*r_decay = &pac->decay_dirty;
		*r_decay_stats = &pac->stats->decay_dirty;
		*r_ecache = &pac->ecache_dirty;
		return;
	case extent_state_muzzy:
		*r_decay = &pac->decay_muzzy;
		*r_decay_stats = &pac->stats->decay_muzzy;
		*r_ecache = &pac->ecache_muzzy;
		return;
	default:
		unreachable();
	}
}



bool
pac_init(tsdn_t *tsdn, pac_t *pac, base_t *base, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms, pac_stats_t *pac_stats, malloc_mutex_t *stats_mtx) {
	unsigned ind = base_ind_get(base);
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

	pac->base = base;
	pac->emap = emap;
	pac->edata_cache = edata_cache;
	pac->stats = pac_stats;
	pac->stats_mtx = stats_mtx;
	atomic_store_zu(&pac->extent_sn_next, 0, ATOMIC_RELAXED);
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

static size_t
pac_stash_decayed(tsdn_t *tsdn, pac_t *pac, ecache_t *ecache,
    size_t npages_limit, size_t npages_decay_max, edata_list_t *result) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 0);
	ehooks_t *ehooks = pac_ehooks_get(pac);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	while (nstashed < npages_decay_max) {
		edata_t *edata = ecache_evict(tsdn, pac, ehooks, ecache,
		    npages_limit);
		if (edata == NULL) {
			break;
		}
		edata_list_append(result, edata);
		nstashed += edata_size_get(edata) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
pac_decay_stashed(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    edata_list_t *decay_extents) {
	bool err;

	size_t nmadvise = 0;
	size_t nunmapped = 0;
	size_t npurged = 0;

	ehooks_t *ehooks = pac_ehooks_get(pac);

	bool try_muzzy = !fully_decay
	    && pac_decay_ms_get(pac, extent_state_muzzy) != 0;

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
					ecache_dalloc(tsdn, pac, ehooks,
					    &pac->ecache_muzzy, edata);
					break;
				}
			}
			JEMALLOC_FALLTHROUGH;
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, pac, ehooks, edata);
			nunmapped += npages;
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		LOCKEDINT_MTX_LOCK(tsdn, *pac->stats_mtx);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->npurge, 1);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->nmadvise, nmadvise);
		locked_inc_u64(tsdn, LOCKEDINT_MTX(*pac->stats_mtx),
		    &decay_stats->purged, npurged);
		LOCKEDINT_MTX_UNLOCK(tsdn, *pac->stats_mtx);
		atomic_fetch_sub_zu(&pac->stats->pac_mapped,
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
pac_decay_to_limit(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay,
    size_t npages_limit, size_t npages_decay_max) {
	witness_assert_depth_to_rank(tsdn_witness_tsdp_get(tsdn),
	    WITNESS_RANK_CORE, 1);

	if (decay->purging || npages_decay_max == 0) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	edata_list_t decay_extents;
	edata_list_init(&decay_extents);
	size_t npurge = pac_stash_decayed(tsdn, pac, ecache, npages_limit,
	    npages_decay_max, &decay_extents);
	if (npurge != 0) {
		size_t npurged = pac_decay_stashed(tsdn, pac, decay,
		    decay_stats, ecache, fully_decay, &decay_extents);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

void
pac_decay_all(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);
	pac_decay_to_limit(tsdn, pac, decay, decay_stats, ecache, fully_decay,
	    /* npages_limit */ 0, ecache_npages_get(ecache));
}

static void
pac_decay_try_purge(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    size_t current_npages, size_t npages_limit) {
	if (current_npages > npages_limit) {
		pac_decay_to_limit(tsdn, pac, decay, decay_stats, ecache,
		    /* fully_decay */ false, npages_limit,
		    current_npages - npages_limit);
	}
}

bool
pac_maybe_decay_purge(tsdn_t *tsdn, pac_t *pac, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    pac_purge_eagerness_t eagerness) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	/* Purge all or nothing if the option is disabled. */
	ssize_t decay_ms = decay_ms_read(decay);
	if (decay_ms <= 0) {
		if (decay_ms == 0) {
			pac_decay_to_limit(tsdn, pac, decay, decay_stats,
			    ecache, /* fully_decay */ false,
			    /* npages_limit */ 0, ecache_npages_get(ecache));
		}
		return false;
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances, or
	 * being triggered by background threads (scheduled event).
	 */
	nstime_t time;
	nstime_init_update(&time);
	size_t npages_current = ecache_npages_get(ecache);
	bool epoch_advanced = decay_maybe_advance_epoch(decay, &time,
	    npages_current);
	if (eagerness == PAC_PURGE_ALWAYS
	    || (epoch_advanced && eagerness == PAC_PURGE_ON_EPOCH_ADVANCE)) {
		size_t npages_limit = decay_npages_limit_get(decay);
		pac_decay_try_purge(tsdn, pac, decay, decay_stats, ecache,
		    npages_current, npages_limit);
	}

	return epoch_advanced;
}

bool
pac_decay_ms_set(tsdn_t *tsdn, pac_t *pac, extent_state_t state,
    ssize_t decay_ms, pac_purge_eagerness_t eagerness) {
	decay_t *decay;
	pac_decay_stats_t *decay_stats;
	ecache_t *ecache;
	pac_decay_data_get(pac, state, &decay, &decay_stats, &ecache);

	if (!decay_ms_valid(decay_ms)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_ms changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	nstime_t cur_time;
	nstime_init_update(&cur_time);
	decay_reinit(decay, &cur_time, decay_ms);
	pac_maybe_decay_purge(tsdn, pac, decay, decay_stats, ecache, eagerness);
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return false;
}

ssize_t
pac_decay_ms_get(pac_t *pac, extent_state_t state) {
	decay_t *decay;
	pac_decay_stats_t *decay_stats;
	ecache_t *ecache;
	pac_decay_data_get(pac, state, &decay, &decay_stats, &ecache);
	return decay_ms_read(decay);
}
