#ifndef JEMALLOC_INTERNAL_ARENA_STATS_H
#define JEMALLOC_INTERNAL_ARENA_STATS_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/lockedint.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_prof.h"
#include "jemalloc/internal/pa.h"
#include "jemalloc/internal/sc.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

typedef struct arena_stats_large_s arena_stats_large_t;
struct arena_stats_large_s {
	/*
	 * Total number of allocation/deallocation requests served directly by
	 * the arena.
	 */
	locked_u64_t	nmalloc;
	locked_u64_t	ndalloc;

	/*
	 * Number of allocation requests that correspond to this size class.
	 * This includes requests served by tcache, though tcache only
	 * periodically merges into this counter.
	 */
	locked_u64_t	nrequests; /* Partially derived. */
	/*
	 * Number of tcache fills / flushes for large (similarly, periodically
	 * merged).  Note that there is no large tcache batch-fill currently
	 * (i.e. only fill 1 at a time); however flush may be batched.
	 */
	locked_u64_t	nfills; /* Partially derived. */
	locked_u64_t	nflushes; /* Partially derived. */

	/* Current number of allocations of this size class. */
	size_t		curlextents; /* Derived. */
};

typedef struct arena_stats_decay_s arena_stats_decay_t;
struct arena_stats_decay_s {
	/* Total number of purge sweeps. */
	locked_u64_t	npurge;
	/* Total number of madvise calls made. */
	locked_u64_t	nmadvise;
	/* Total number of pages purged. */
	locked_u64_t	purged;
};

typedef struct arena_stats_extents_s arena_stats_extents_t;
struct arena_stats_extents_s {
	/*
	 * Stats for a given index in the range [0, SC_NPSIZES] in an extents_t.
	 * We track both bytes and # of extents: two extents in the same bucket
	 * may have different sizes if adjacent size classes differ by more than
	 * a page, so bytes cannot always be derived from # of extents.
	 */
	atomic_zu_t ndirty;
	atomic_zu_t dirty_bytes;
	atomic_zu_t nmuzzy;
	atomic_zu_t muzzy_bytes;
	atomic_zu_t nretained;
	atomic_zu_t retained_bytes;
};

/*
 * Arena stats.  Note that fields marked "derived" are not directly maintained
 * within the arena code; rather their values are derived during stats merge
 * requests.
 */
typedef struct arena_stats_s arena_stats_t;
struct arena_stats_s {
	LOCKEDINT_MTX_DECLARE(mtx)

	/*
	 * Number of bytes currently mapped, excluding retained memory.
	 */
	locked_zu_t		mapped; /* Partially derived. */

	/*
	 * Number of unused virtual memory bytes currently retained.  Retained
	 * bytes are technically mapped (though always decommitted or purged),
	 * but they are excluded from the mapped statistic (above).
	 */
	locked_zu_t		retained; /* Derived. */

	/* Number of edata_t structs allocated by base, but not being used. */
	atomic_zu_t		edata_avail;

	arena_stats_decay_t	decay_dirty;
	arena_stats_decay_t	decay_muzzy;

	atomic_zu_t		base; /* Derived. */
	atomic_zu_t		internal;
	atomic_zu_t		resident; /* Derived. */
	atomic_zu_t		metadata_thp;

	atomic_zu_t		allocated_large; /* Derived. */
	locked_u64_t	nmalloc_large; /* Derived. */
	locked_u64_t	ndalloc_large; /* Derived. */
	locked_u64_t	nfills_large; /* Derived. */
	locked_u64_t	nflushes_large; /* Derived. */
	locked_u64_t	nrequests_large; /* Derived. */

	/*
	 * The stats logically owned by the pa_shard in the same arena.  This
	 * lives here only because it's convenient for the purposes of the ctl
	 * module -- it only knows about the single arena_stats.
	 */
	pa_shard_stats_t	pa_shard_stats;

	/* Number of bytes cached in tcache associated with this arena. */
	atomic_zu_t		tcache_bytes; /* Derived. */

	mutex_prof_data_t mutex_prof_data[mutex_prof_num_arena_mutexes];

	/* One element for each large size class. */
	arena_stats_large_t	lstats[SC_NSIZES - SC_NBINS];

	/* Arena uptime. */
	nstime_t		uptime;
};

static inline bool
arena_stats_init(tsdn_t *tsdn, arena_stats_t *arena_stats) {
	if (config_debug) {
		for (size_t i = 0; i < sizeof(arena_stats_t); i++) {
			assert(((char *)arena_stats)[i] == 0);
		}
	}
	if (LOCKEDINT_MTX_INIT(LOCKEDINT_MTX(arena_stats->mtx), "arena_stats",
	    WITNESS_RANK_ARENA_STATS, malloc_mutex_rank_exclusive)) {
		return true;
	}
	/* Memory is zeroed, so there is no need to clear stats. */
	return false;
}

static inline void
arena_stats_large_flush_nrequests_add(tsdn_t *tsdn, arena_stats_t *arena_stats,
    szind_t szind, uint64_t nrequests) {
	LOCKEDINT_MTX_LOCK(tsdn, arena_stats->mtx);
	arena_stats_large_t *lstats = &arena_stats->lstats[szind - SC_NBINS];
	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena_stats->mtx),
	    &lstats->nrequests, nrequests);
	locked_inc_u64(tsdn, LOCKEDINT_MTX(arena_stats->mtx),
	    &lstats->nflushes, 1);
	LOCKEDINT_MTX_UNLOCK(tsdn, arena_stats->mtx);
}

static inline void
arena_stats_mapped_add(tsdn_t *tsdn, arena_stats_t *arena_stats, size_t size) {
	LOCKEDINT_MTX_LOCK(tsdn, arena_stats->mtx);
	locked_inc_zu(tsdn, LOCKEDINT_MTX(arena_stats->mtx),
	    &arena_stats->mapped, size);
	LOCKEDINT_MTX_UNLOCK(tsdn, arena_stats->mtx);
}

#endif /* JEMALLOC_INTERNAL_ARENA_STATS_H */
