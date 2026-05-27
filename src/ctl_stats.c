#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl_arena.h"
#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/ctl_stats.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_recent.h"
#include "jemalloc/internal/prof_stats.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* stats.* ctl state. */

static ctl_stats_t *ctl_stats;

bool
ctl_stats_init(tsdn_t *tsdn) {
	if (!config_stats || ctl_stats != NULL) {
		return false;
	}

	ctl_stats = (ctl_stats_t *)base_alloc(
	    tsdn, b0get(), sizeof(ctl_stats_t), QUANTUM);
	return ctl_stats == NULL;
}

static void
ctl_background_thread_stats_read(tsdn_t *tsdn) {
	background_thread_stats_t *stats = &ctl_stats->background_thread;
	if (!have_background_thread
	    || background_thread_stats_read(tsdn, stats)) {
		memset(stats, 0, sizeof(background_thread_stats_t));
		nstime_init_zero(&stats->run_interval);
	}
	malloc_mutex_prof_copy(
	    &ctl_stats->mutex_prof_data[global_prof_mutex_max_per_bg_thd],
	    &stats->max_counter_per_bg_thd);
}

void
ctl_stats_refresh(tsdn_t *tsdn, ctl_arena_t *ctl_sarena) {
	if (!config_stats) {
		return;
	}

	ctl_stats->allocated = ctl_sarena->astats->allocated_small
	    + ctl_sarena->astats->astats.allocated_large;
	ctl_stats->active = (ctl_sarena->pactive << LG_PAGE);
	ctl_stats->metadata = ctl_sarena->astats->astats.base
	    + atomic_load_zu(
	        &ctl_sarena->astats->astats.internal, ATOMIC_RELAXED);
	ctl_stats->metadata_edata = ctl_sarena->astats->astats.metadata_edata;
	ctl_stats->metadata_rtree = ctl_sarena->astats->astats.metadata_rtree;
	ctl_stats->resident = ctl_sarena->astats->astats.resident;
	ctl_stats->metadata_thp = ctl_sarena->astats->astats.metadata_thp;
	ctl_stats->mapped = ctl_sarena->astats->astats.mapped;
	ctl_stats->retained = ctl_sarena->astats->astats.pa_shard_stats
	                          .pac_stats.retained;
	ctl_stats->pinned = ctl_sarena->astats->astats.pa_shard_stats
	                        .pac_stats.pinned;

	ctl_background_thread_stats_read(tsdn);

#define READ_GLOBAL_MUTEX_PROF_DATA(i, mtx)                                    \
	malloc_mutex_lock(tsdn, &mtx);                                         \
	malloc_mutex_prof_read(tsdn, &ctl_stats->mutex_prof_data[i], &mtx);    \
	malloc_mutex_unlock(tsdn, &mtx);

	if (config_prof && opt_prof) {
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof, bt2gctx_mtx);
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof_thds_data, tdatas_mtx);
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof_dump, prof_dump_mtx);
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof_recent_alloc,
		    prof_recent_alloc_mtx);
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof_recent_dump,
		    prof_recent_dump_mtx);
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_prof_stats, prof_stats_mtx);
	}
	if (have_background_thread) {
		READ_GLOBAL_MUTEX_PROF_DATA(
		    global_prof_mutex_background_thread, background_thread_lock);
	} else {
		memset(&ctl_stats->mutex_prof_data
		           [global_prof_mutex_background_thread],
		    0, sizeof(mutex_prof_data_t));
	}
	ctl_mtx_prof_read(
	    tsdn, &ctl_stats->mutex_prof_data[global_prof_mutex_ctl]);
#undef READ_GLOBAL_MUTEX_PROF_DATA
}

/******************************************************************************/
/* stats.* mallctl handlers. */

CTL_RO_CGEN_PUBLIC(config_stats, stats_allocated, ctl_stats->allocated, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_active, ctl_stats->active, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_metadata, ctl_stats->metadata, size_t)
CTL_RO_CGEN_PUBLIC(
    config_stats, stats_metadata_edata, ctl_stats->metadata_edata, size_t)
CTL_RO_CGEN_PUBLIC(
    config_stats, stats_metadata_rtree, ctl_stats->metadata_rtree, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_metadata_thp, ctl_stats->metadata_thp, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_resident, ctl_stats->resident, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_mapped, ctl_stats->mapped, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_retained, ctl_stats->retained, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_pinned, ctl_stats->pinned, size_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_background_thread_num_threads,
    ctl_stats->background_thread.num_threads, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_background_thread_num_runs,
    ctl_stats->background_thread.num_runs, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_background_thread_run_interval,
    nstime_ns(&ctl_stats->background_thread.run_interval), uint64_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_zero_reallocs,
    atomic_load_zu(&zero_realloc_count, ATOMIC_RELAXED), size_t)

/*
 * approximate_stats.active returns a result that is informative itself,
 * but the returned value SHOULD NOT be compared against other stats retrieved.
 * For instance, approximate_stats.active should not be compared against
 * any stats, e.g., stats.active or stats.resident, because there is no
 * guarantee in the comparison results.  Results returned by stats.*, on the
 * other hand, provides such guarantees, i.e., stats.active <= stats.resident,
 * as long as epoch is called right before the queries.
 */

int
approximate_stats_active_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		tsdn_t  *tsdn = tsd_tsdn(tsd);
		unsigned n = narenas_total_get();

		size_t approximate_nactive = 0;
		for (unsigned i = 0; i < n; i++) {
			arena_t *arena = arena_get(tsdn, i, false);
			if (!arena) {
				continue;
			}
			/* Accumulate nactive pages from each arena's pa_shard */
			approximate_nactive +=
			    pa_shard_nactive(&arena->pa_shard);
		}

		size_t approximate_active_bytes = approximate_nactive << LG_PAGE;
		ret = ctl_read(oldp, oldlenp, &approximate_active_bytes,
		    sizeof(approximate_active_bytes));
	}
	return ret;
}

CTL_RO_GEN_PUBLIC(stats_arenas_i_dss, ctl_arenas_i(mib[2])->dss, const char *)
CTL_RO_GEN_PUBLIC(
    stats_arenas_i_dirty_decay_ms, ctl_arenas_i(mib[2])->dirty_decay_ms, ssize_t)
CTL_RO_GEN_PUBLIC(
    stats_arenas_i_muzzy_decay_ms, ctl_arenas_i(mib[2])->muzzy_decay_ms, ssize_t)
CTL_RO_GEN_PUBLIC(stats_arenas_i_nthreads, ctl_arenas_i(mib[2])->nthreads, unsigned)
CTL_RO_GEN_PUBLIC(stats_arenas_i_uptime,
    nstime_ns(&ctl_arenas_i(mib[2])->astats->astats.uptime), uint64_t)
CTL_RO_GEN_PUBLIC(stats_arenas_i_pactive, ctl_arenas_i(mib[2])->pactive, size_t)
CTL_RO_GEN_PUBLIC(stats_arenas_i_pdirty, ctl_arenas_i(mib[2])->pdirty, size_t)
CTL_RO_GEN_PUBLIC(stats_arenas_i_pmuzzy, ctl_arenas_i(mib[2])->pmuzzy, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_mapped,
    ctl_arenas_i(mib[2])->astats->astats.mapped, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_retained,
    ctl_arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.retained, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_pinned,
    ctl_arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.pinned, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extent_avail,
    ctl_arenas_i(mib[2])->astats->astats.pa_shard_stats.edata_avail, size_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_dirty_npurge,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_dirty.npurge),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_dirty_nmadvise,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_dirty.nmadvise),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_dirty_purged,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_dirty.purged),
    uint64_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_muzzy_npurge,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.npurge),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_muzzy_nmadvise,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.nmadvise),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_muzzy_purged,
    locked_read_u64_unsynchronized(&ctl_arenas_i(mib[2])
            ->astats->astats.pa_shard_stats.pac_stats.decay_muzzy.purged),
    uint64_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_base,
    ctl_arenas_i(mib[2])->astats->astats.base, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_internal,
    atomic_load_zu(&ctl_arenas_i(mib[2])->astats->astats.internal, ATOMIC_RELAXED),
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_metadata_edata,
    ctl_arenas_i(mib[2])->astats->astats.metadata_edata, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_metadata_rtree,
    ctl_arenas_i(mib[2])->astats->astats.metadata_rtree, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_metadata_thp,
    ctl_arenas_i(mib[2])->astats->astats.metadata_thp, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_tcache_bytes,
    ctl_arenas_i(mib[2])->astats->astats.tcache_bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_tcache_stashed_bytes,
    ctl_arenas_i(mib[2])->astats->astats.tcache_stashed_bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_resident,
    ctl_arenas_i(mib[2])->astats->astats.resident, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_abandoned_vm,
    atomic_load_zu(
        &ctl_arenas_i(mib[2])->astats->astats.pa_shard_stats.pac_stats.abandoned_vm,
        ATOMIC_RELAXED),
    size_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_bytes,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_hits,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.total.nhits, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_misses,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.total.nmisses, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_dalloc_flush,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.total.ndalloc_flush, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_dalloc_noflush,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.total.ndalloc_noflush, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_sec_overfills,
    ctl_arenas_i(mib[2])->astats->hpastats.secstats.total.noverfills, size_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_allocated,
    ctl_arenas_i(mib[2])->astats->allocated_small, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_nmalloc,
    ctl_arenas_i(mib[2])->astats->nmalloc_small, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_ndalloc,
    ctl_arenas_i(mib[2])->astats->ndalloc_small, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_nrequests,
    ctl_arenas_i(mib[2])->astats->nrequests_small, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_nfills,
    ctl_arenas_i(mib[2])->astats->nfills_small, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_small_nflushes,
    ctl_arenas_i(mib[2])->astats->nflushes_small, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_allocated,
    ctl_arenas_i(mib[2])->astats->astats.allocated_large, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_nmalloc,
    ctl_arenas_i(mib[2])->astats->astats.nmalloc_large, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_ndalloc,
    ctl_arenas_i(mib[2])->astats->astats.ndalloc_large, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_nrequests,
    ctl_arenas_i(mib[2])->astats->astats.nrequests_large, uint64_t)
/*
 * Note: "nmalloc_large" here instead of "nfills" in the read.  This is
 * intentional (large has no batch fill).
 */
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_nfills,
    ctl_arenas_i(mib[2])->astats->astats.nmalloc_large, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_large_nflushes,
    ctl_arenas_i(mib[2])->astats->astats.nflushes_large, uint64_t)

/* Lock profiling related APIs below. */
#define RO_MUTEX_CTL_GEN(n, l)                                                 \
	CTL_RO_CGEN_PUBLIC(config_stats, stats_##n##_num_ops, l.n_lock_ops, uint64_t) \
	CTL_RO_CGEN_PUBLIC(                                                           \
	    config_stats, stats_##n##_num_wait, l.n_wait_times, uint64_t)      \
	CTL_RO_CGEN_PUBLIC(config_stats, stats_##n##_num_spin_acq, l.n_spin_acquired, \
	    uint64_t)                                                          \
	CTL_RO_CGEN_PUBLIC(config_stats, stats_##n##_num_owner_switch,                \
	    l.n_owner_switches, uint64_t)                                      \
	CTL_RO_CGEN_PUBLIC(config_stats, stats_##n##_total_wait_time,                 \
	    nstime_ns(&l.tot_wait_time), uint64_t)                             \
	CTL_RO_CGEN_PUBLIC(config_stats, stats_##n##_max_wait_time,                   \
	    nstime_ns(&l.max_wait_time), uint64_t)                             \
	CTL_RO_CGEN_PUBLIC(                                                           \
	    config_stats, stats_##n##_max_num_thds, l.max_n_thds, uint32_t)

/* Global mutexes. */
#define OP(mtx)                                                                \
	RO_MUTEX_CTL_GEN(mutexes_##mtx,                                        \
	    ctl_stats->mutex_prof_data[global_prof_mutex_##mtx])
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP

/* Per arena mutexes */
#define OP(mtx)                                                                \
	RO_MUTEX_CTL_GEN(arenas_i_mutexes_##mtx,                               \
	    ctl_arenas_i(mib[2])                                                   \
	        ->astats->astats.mutex_prof_data[arena_prof_mutex_##mtx])
MUTEX_PROF_ARENA_MUTEXES
#undef OP

/* tcache bin mutex */
RO_MUTEX_CTL_GEN(
    arenas_i_bins_j_mutex, ctl_arenas_i(mib[2])->astats->bstats[mib[4]].mutex_data)
#undef RO_MUTEX_CTL_GEN

/* Resets all mutex stats, including global, arena and bin mutexes. */
int
stats_mutexes_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_stats) {
		return ENOENT;
	}

	tsdn_t *tsdn = tsd_tsdn(tsd);

#define MUTEX_PROF_RESET(mtx)                                                  \
	malloc_mutex_lock(tsdn, &mtx);                                         \
	malloc_mutex_prof_data_reset(tsdn, &mtx);                              \
	malloc_mutex_unlock(tsdn, &mtx);

	/* Global mutexes: ctl and prof. */
	ctl_mtx_prof_data_reset(tsdn);
	if (have_background_thread) {
		MUTEX_PROF_RESET(background_thread_lock);
	}
	if (config_prof && opt_prof) {
		MUTEX_PROF_RESET(bt2gctx_mtx);
		MUTEX_PROF_RESET(tdatas_mtx);
		MUTEX_PROF_RESET(prof_dump_mtx);
		MUTEX_PROF_RESET(prof_recent_alloc_mtx);
		MUTEX_PROF_RESET(prof_recent_dump_mtx);
		MUTEX_PROF_RESET(prof_stats_mtx);
	}

	/* Per arena mutexes. */
	unsigned n = narenas_total_get();

	for (unsigned i = 0; i < n; i++) {
		arena_t *arena = arena_get(tsdn, i, false);
		if (!arena) {
			continue;
		}
		MUTEX_PROF_RESET(arena->large_mtx);
		MUTEX_PROF_RESET(arena->pa_shard.edata_cache.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_dirty.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_muzzy.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_retained.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.ecache_pinned.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.decay_dirty.mtx);
		MUTEX_PROF_RESET(arena->pa_shard.pac.decay_muzzy.mtx);
		MUTEX_PROF_RESET(arena->cache_bin_array_descriptor_ql_mtx);
		MUTEX_PROF_RESET(arena->base->mtx);

		for (szind_t j = 0; j < SC_NBINS; j++) {
			for (unsigned k = 0; k < bin_infos[j].n_shards; k++) {
				bin_t *bin = arena_get_bin(arena, j, k);
				MUTEX_PROF_RESET(bin->lock);
			}
		}
	}
#undef MUTEX_PROF_RESET
	return 0;
}

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nmalloc,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nmalloc, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_ndalloc,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.ndalloc, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nrequests,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nrequests, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_curregs,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.curregs, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nfills,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nfills, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nflushes,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nflushes, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nslabs,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nslabs, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nreslabs,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.reslabs, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_curslabs,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.curslabs, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_bins_j_nonfull_slabs,
    ctl_arenas_i(mib[2])->astats->bstats[mib[4]].stats_data.nonfull_slabs, size_t)


CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_lextents_j_nmalloc,
    locked_read_u64_unsynchronized(
        &ctl_arenas_i(mib[2])->astats->lstats[mib[4]].nmalloc),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_lextents_j_ndalloc,
    locked_read_u64_unsynchronized(
        &ctl_arenas_i(mib[2])->astats->lstats[mib[4]].ndalloc),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_lextents_j_nrequests,
    locked_read_u64_unsynchronized(
        &ctl_arenas_i(mib[2])->astats->lstats[mib[4]].nrequests),
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_lextents_j_curlextents,
    ctl_arenas_i(mib[2])->astats->lstats[mib[4]].curlextents, size_t)


CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_ndirty,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].ndirty, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_nmuzzy,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].nmuzzy, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_nretained,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].nretained, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_npinned,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].npinned, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_dirty_bytes,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].dirty_bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_muzzy_bytes,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].muzzy_bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_retained_bytes,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].retained_bytes, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_extents_j_pinned_bytes,
    ctl_arenas_i(mib[2])->astats->estats[mib[4]].pinned_bytes, size_t)


CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_npageslabs,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.merged.npageslabs, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_nactive,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.merged.nactive, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_ndirty,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.merged.ndirty, size_t)

/* Nonhuge slabs */
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_npageslabs_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[0].npageslabs, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_nactive_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[0].nactive, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_ndirty_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[0].ndirty, size_t)

/* Huge slabs */
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_npageslabs_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[1].npageslabs, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_nactive_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[1].nactive, size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_slabs_ndirty_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.slabs[1].ndirty, size_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_npurge_passes,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.npurge_passes,
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_npurges,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.npurges, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_nhugifies,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.nhugifies, uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_nhugify_failures,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.nhugify_failures,
    uint64_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_ndehugifies,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.ndehugifies, uint64_t)

CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_min_extents,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats.hpa_alloc_min_extents[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_max_extents,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats.hpa_alloc_max_extents[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_extents,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats.hpa_alloc_extents[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_ps,
    ctl_arenas_i(mib[2])->astats->hpastats.nonderived_stats.hpa_alloc_ps[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_pages_per_ps,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats.hpa_alloc_pages_per_ps[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_alloc_j_extents_per_ps,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats.hpa_alloc_extents_per_ps[mib[5]],
    uint64_t);
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_alloc_j_total_elapsed_ns_per_ps,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.nonderived_stats
        .hpa_alloc_total_elapsed_ns_per_ps[mib[5]],
    uint64_t);

/* Full, nonhuge */
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_full_slabs_npageslabs_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_full_slabs_nactive_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_full_slabs_ndirty_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[0].ndirty,
    size_t)

/* Full, huge */
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_full_slabs_npageslabs_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_full_slabs_nactive_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_full_slabs_ndirty_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.full_slabs[1].ndirty,
    size_t)

/* Empty, nonhuge */
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_empty_slabs_npageslabs_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_empty_slabs_nactive_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_empty_slabs_ndirty_nonhuge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[0].ndirty,
    size_t)

/* Empty, huge */
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_empty_slabs_npageslabs_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_empty_slabs_nactive_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_empty_slabs_ndirty_huge,
    ctl_arenas_i(mib[2])->astats->hpastats.psset_stats.empty_slabs[1].ndirty,
    size_t)

/* Nonfull, nonhuge */
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_nonhuge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0]
        .npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_nonhuge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0]
        .nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_nonhuge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][0]
        .ndirty,
    size_t)

/* Nonfull, huge */
CTL_RO_CGEN_PUBLIC(config_stats,
    stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_huge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1]
        .npageslabs,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_huge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1]
        .nactive,
    size_t)
CTL_RO_CGEN_PUBLIC(config_stats, stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_huge,
    ctl_arenas_i(mib[2])
        ->astats->hpastats.psset_stats.nonfull_slabs[mib[5]][1]
        .ndirty,
    size_t)
