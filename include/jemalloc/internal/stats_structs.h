#ifndef JEMALLOC_INTERNAL_STATS_STRUCTS_H
#define JEMALLOC_INTERNAL_STATS_STRUCTS_H

#ifdef JEMALLOC_ATOMIC_U64
typedef atomic_u64_t arena_stats_u64_t;
#else
/* Must hold the arena stats mutex while reading atomically. */
typedef uint64_t arena_stats_u64_t;
#endif

struct tcache_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t	nrequests;
};

struct malloc_bin_stats_s {
	/*
	 * Total number of allocation/deallocation requests served directly by
	 * the bin.  Note that tcache may allocate an object, then recycle it
	 * many times, resulting many increments to nrequests, but only one
	 * each to nmalloc and ndalloc.
	 */
	uint64_t	nmalloc;
	uint64_t	ndalloc;

	/*
	 * Number of allocation requests that correspond to the size of this
	 * bin.  This includes requests served by tcache, though tcache only
	 * periodically merges into this counter.
	 */
	uint64_t	nrequests;

	/*
	 * Current number of regions of this size class, including regions
	 * currently cached by tcache.
	 */
	size_t		curregs;

	/* Number of tcache fills from this bin. */
	uint64_t	nfills;

	/* Number of tcache flushes to this bin. */
	uint64_t	nflushes;

	/* Total number of slabs created for this bin's size class. */
	uint64_t	nslabs;

	/*
	 * Total number of slabs reused by extracting them from the slabs heap
	 * for this bin's size class.
	 */
	uint64_t	reslabs;

	/* Current number of slabs in this bin. */
	size_t		curslabs;
};

struct malloc_large_stats_s {
	/*
	 * Total number of allocation/deallocation requests served directly by
	 * the arena.
	 */
	arena_stats_u64_t	nmalloc;
	arena_stats_u64_t	ndalloc;

	/*
	 * Number of allocation requests that correspond to this size class.
	 * This includes requests served by tcache, though tcache only
	 * periodically merges into this counter.
	 */
	arena_stats_u64_t	nrequests; /* Partially derived. */

	/* Current number of allocations of this size class. */
	size_t		curlextents; /* Derived. */
};

struct decay_stats_s {
	/* Total number of purge sweeps. */
	arena_stats_u64_t	npurge;
	/* Total number of madvise calls made. */
	arena_stats_u64_t	nmadvise;
	/* Total number of pages purged. */
	arena_stats_u64_t	purged;
};

/*
 * Arena stats.  Note that fields marked "derived" are not directly maintained
 * within the arena code; rather their values are derived during stats merge
 * requests.
 */
struct arena_stats_s {
#ifndef JEMALLOC_ATOMIC_U64
	malloc_mutex_t		mtx;
#endif

	/* Number of bytes currently mapped, excluding retained memory. */
	atomic_zu_t		mapped; /* Partially derived. */

	/*
	 * Number of bytes currently retained as a side effect of munmap() being
	 * disabled/bypassed.  Retained bytes are technically mapped (though
	 * always decommitted or purged), but they are excluded from the mapped
	 * statistic (above).
	 */
	atomic_zu_t		retained; /* Derived. */

	decay_stats_t		decay_dirty;
	decay_stats_t		decay_muzzy;

	atomic_zu_t		base; /* Derived. */
	atomic_zu_t		internal;
	atomic_zu_t		resident; /* Derived. */

	atomic_zu_t		allocated_large; /* Derived. */
	arena_stats_u64_t	nmalloc_large; /* Derived. */
	arena_stats_u64_t	ndalloc_large; /* Derived. */
	arena_stats_u64_t	nrequests_large; /* Derived. */

	/* Number of bytes cached in tcache associated with this arena. */
	atomic_zu_t		tcache_bytes; /* Derived. */

	/* One element for each large size class. */
	malloc_large_stats_t	lstats[NSIZES - NBINS];
};

#endif /* JEMALLOC_INTERNAL_STATS_STRUCTS_H */
