#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/pa.h"

/*
 * This file is logically part of the PA module.  While pa.c contains the core
 * allocator functionality, this file contains boring integration functionality;
 * things like the pre- and post- fork handlers, and stats merging for CTL
 * refreshes.
 */

void
pa_shard_prefork0(tsdn_t *tsdn, pa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->pac.decay_dirty.mtx);
	malloc_mutex_prefork(tsdn, &shard->pac.decay_muzzy.mtx);
}

void
pa_shard_prefork2(tsdn_t *tsdn, pa_shard_t *shard) {
	sec_prefork2(tsdn, &shard->pac.sec);
	if (shard->ever_used_hpa) {
		hpa_shard_prefork2(tsdn, &shard->hpa);
	}
}

void
pa_shard_prefork3(tsdn_t *tsdn, pa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->pac.grow_mtx);
	if (shard->ever_used_hpa) {
		hpa_shard_prefork3(tsdn, &shard->hpa);
	}
}

void
pa_shard_prefork4(tsdn_t *tsdn, pa_shard_t *shard) {
	ecache_prefork(tsdn, &shard->pac.ecache_dirty);
	ecache_prefork(tsdn, &shard->pac.ecache_muzzy);
	ecache_prefork(tsdn, &shard->pac.ecache_retained);
	ecache_prefork(tsdn, &shard->pac.ecache_pinned);
	if (shard->ever_used_hpa) {
		hpa_shard_prefork4(tsdn, &shard->hpa);
	}
}

void
pa_shard_prefork5(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_prefork(tsdn, &shard->edata_cache);
}

void
pa_shard_postfork_parent(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_postfork_parent(tsdn, &shard->edata_cache);
	ecache_postfork_parent(tsdn, &shard->pac.ecache_dirty);
	ecache_postfork_parent(tsdn, &shard->pac.ecache_muzzy);
	ecache_postfork_parent(tsdn, &shard->pac.ecache_retained);
	ecache_postfork_parent(tsdn, &shard->pac.ecache_pinned);
	malloc_mutex_postfork_parent(tsdn, &shard->pac.grow_mtx);
	sec_postfork_parent(tsdn, &shard->pac.sec);
	malloc_mutex_postfork_parent(tsdn, &shard->pac.decay_dirty.mtx);
	malloc_mutex_postfork_parent(tsdn, &shard->pac.decay_muzzy.mtx);
	if (shard->ever_used_hpa) {
		hpa_shard_postfork_parent(tsdn, &shard->hpa);
	}
}

void
pa_shard_postfork_child(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_postfork_child(tsdn, &shard->edata_cache);
	ecache_postfork_child(tsdn, &shard->pac.ecache_dirty);
	ecache_postfork_child(tsdn, &shard->pac.ecache_muzzy);
	ecache_postfork_child(tsdn, &shard->pac.ecache_retained);
	ecache_postfork_child(tsdn, &shard->pac.ecache_pinned);
	malloc_mutex_postfork_child(tsdn, &shard->pac.grow_mtx);
	sec_postfork_child(tsdn, &shard->pac.sec);
	malloc_mutex_postfork_child(tsdn, &shard->pac.decay_dirty.mtx);
	malloc_mutex_postfork_child(tsdn, &shard->pac.decay_muzzy.mtx);
	if (shard->ever_used_hpa) {
		hpa_shard_postfork_child(tsdn, &shard->hpa);
	}
}

size_t
pa_shard_nactive(const pa_shard_t *shard) {
	return atomic_load_zu(&shard->nactive, ATOMIC_RELAXED);
}

static size_t
pac_sec_pinned_bytes_get(const sec_stats_t *stats) {
	return min_zu(stats->bytes_pinned, stats->bytes);
}

static size_t
pac_sec_dirty_npages_get(const sec_stats_t *stats) {
	return (stats->bytes - pac_sec_pinned_bytes_get(stats)) >> LG_PAGE;
}

static size_t
pa_shard_ndirty_no_pac_sec(const pa_shard_t *shard) {
	size_t ndirty = ecache_npages_get(&shard->pac.ecache_dirty);
	if (shard->ever_used_hpa) {
		ndirty += psset_ndirty(&shard->hpa.psset);
	}
	return ndirty;
}

static size_t
pa_shard_ndirty(const pa_shard_t *shard) {
	sec_stats_t pac_sec_stats = {0};
	sec_stats_merge(TSDN_NULL, &shard->pac.sec, &pac_sec_stats);
	return pa_shard_ndirty_no_pac_sec(shard)
	    + pac_sec_dirty_npages_get(&pac_sec_stats);
}

static size_t
pa_shard_nmuzzy(const pa_shard_t *shard) {
	return ecache_npages_get(&shard->pac.ecache_muzzy);
}

void
pa_shard_basic_stats_merge(
    const pa_shard_t *shard, size_t *nactive, size_t *ndirty, size_t *nmuzzy) {
	*nactive += pa_shard_nactive(shard);
	*ndirty += pa_shard_ndirty(shard);
	*nmuzzy += pa_shard_nmuzzy(shard);
}

void
pa_shard_stats_merge(tsdn_t *tsdn, pa_shard_t *shard,
    pa_shard_stats_t *pa_shard_stats_out, pac_estats_t *estats_out,
    hpa_shard_stats_t *hpa_stats_out, size_t *resident) {
	cassert(config_stats);

	sec_stats_t pac_sec_stats = {0};
	sec_stats_merge(tsdn, &shard->pac.sec, &pac_sec_stats);
	sec_stats_accum(&pa_shard_stats_out->pac_stats.pac_sec_stats,
	    &pac_sec_stats);
	size_t pac_sec_pinned_bytes = pac_sec_pinned_bytes_get(&pac_sec_stats);
	size_t pac_sec_pinned_npages = pac_sec_pinned_bytes >> LG_PAGE;
	size_t pac_sec_dirty_npages =
	    pac_sec_dirty_npages_get(&pac_sec_stats);

	pa_shard_stats_out->pac_stats.retained +=
	    ecache_npages_get(&shard->pac.ecache_retained) << LG_PAGE;
	pa_shard_stats_out->pac_stats.pinned +=
	    (ecache_npages_get(&shard->pac.ecache_pinned) << LG_PAGE)
	    + pac_sec_pinned_bytes;
	pa_shard_stats_out->edata_avail += atomic_load_zu(
	    &shard->edata_cache.count, ATOMIC_RELAXED);

	size_t resident_pgs = 0;
	resident_pgs += pa_shard_nactive(shard);
	resident_pgs += pa_shard_ndirty_no_pac_sec(shard);
	resident_pgs += pac_sec_dirty_npages;
	resident_pgs += ecache_npages_get(&shard->pac.ecache_pinned);
	resident_pgs += pac_sec_pinned_npages;
	*resident += (resident_pgs << LG_PAGE);

	/* Dirty decay stats */
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_dirty.npurge,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_dirty.npurge));
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_dirty.nmadvise,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_dirty.nmadvise));
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_dirty.purged,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_dirty.purged));

	/* Muzzy decay stats */
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_muzzy.npurge,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_muzzy.npurge));
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_muzzy.nmadvise,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_muzzy.nmadvise));
	locked_inc_u64_unsynchronized(
	    &pa_shard_stats_out->pac_stats.decay_muzzy.purged,
	    locked_read_u64(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	        &shard->pac.stats->decay_muzzy.purged));

	atomic_load_add_store_zu(&pa_shard_stats_out->pac_stats.abandoned_vm,
	    atomic_load_zu(&shard->pac.stats->abandoned_vm, ATOMIC_RELAXED));

	for (pszind_t i = 0; i < SC_NPSIZES; i++) {
		size_t dirty, muzzy, retained, pinned, dirty_bytes,
		    muzzy_bytes, retained_bytes, pinned_bytes;
		dirty = ecache_nextents_get(&shard->pac.ecache_dirty, i);
		muzzy = ecache_nextents_get(&shard->pac.ecache_muzzy, i);
		retained = ecache_nextents_get(&shard->pac.ecache_retained, i);
		pinned = ecache_nextents_get(&shard->pac.ecache_pinned, i);
		dirty_bytes = ecache_nbytes_get(&shard->pac.ecache_dirty, i);
		muzzy_bytes = ecache_nbytes_get(&shard->pac.ecache_muzzy, i);
		retained_bytes = ecache_nbytes_get(
		    &shard->pac.ecache_retained, i);
		pinned_bytes = ecache_nbytes_get(
		    &shard->pac.ecache_pinned, i);

		sec_pszind_stats_t pac_sec_pszind_stats = {0};
		sec_stats_merge_pszind(
		    tsdn, &shard->pac.sec, i, &pac_sec_pszind_stats);
		dirty += pac_sec_pszind_stats.nextents
		    - pac_sec_pszind_stats.nextents_pinned;
		dirty_bytes += pac_sec_pszind_stats.bytes
		    - pac_sec_pszind_stats.bytes_pinned;
		pinned += pac_sec_pszind_stats.nextents_pinned;
		pinned_bytes += pac_sec_pszind_stats.bytes_pinned;

		estats_out[i].ndirty = dirty;
		estats_out[i].nmuzzy = muzzy;
		estats_out[i].nretained = retained;
		estats_out[i].npinned = pinned;
		estats_out[i].dirty_bytes = dirty_bytes;
		estats_out[i].muzzy_bytes = muzzy_bytes;
		estats_out[i].retained_bytes = retained_bytes;
		estats_out[i].pinned_bytes = pinned_bytes;
	}

	if (shard->ever_used_hpa) {
		hpa_shard_stats_merge(tsdn, &shard->hpa, hpa_stats_out);
	}
}

static void
pa_shard_mtx_stats_read_single(tsdn_t *tsdn, mutex_prof_data_t *mutex_prof_data,
    malloc_mutex_t *mtx, int ind) {
	malloc_mutex_lock(tsdn, mtx);
	malloc_mutex_prof_read(tsdn, &mutex_prof_data[ind], mtx);
	malloc_mutex_unlock(tsdn, mtx);
}

void
pa_shard_mtx_stats_read(tsdn_t *tsdn, pa_shard_t *shard,
    mutex_prof_data_t mutex_prof_data[mutex_prof_num_arena_mutexes]) {
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->edata_cache.mtx, arena_prof_mutex_extent_avail);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.ecache_dirty.mtx, arena_prof_mutex_extents_dirty);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.ecache_muzzy.mtx, arena_prof_mutex_extents_muzzy);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.ecache_retained.mtx, arena_prof_mutex_extents_retained);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.ecache_pinned.mtx, arena_prof_mutex_extents_pinned);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.decay_dirty.mtx, arena_prof_mutex_decay_dirty);
	pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
	    &shard->pac.decay_muzzy.mtx, arena_prof_mutex_decay_muzzy);

	sec_mutex_stats_read(tsdn, &shard->pac.sec,
	    &mutex_prof_data[arena_prof_mutex_pac_sec]);

	if (shard->ever_used_hpa) {
		pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
		    &shard->hpa.mtx, arena_prof_mutex_hpa_shard);
		pa_shard_mtx_stats_read_single(tsdn, mutex_prof_data,
		    &shard->hpa.grow_mtx,
		    arena_prof_mutex_hpa_shard_grow);
		sec_mutex_stats_read(tsdn, &shard->hpa.sec,
		    &mutex_prof_data[arena_prof_mutex_hpa_sec]);
	}
}
