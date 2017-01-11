#ifndef JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H
#define JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H
/*
 * Read-only information associated with each element of arena_t's bins array
 * is stored separately, partly to reduce memory usage (only one copy, rather
 * than one per arena), but mainly to avoid false cacheline sharing.
 *
 * Each slab has the following layout:
 *
 *   /--------------------\
 *   | region 0           |
 *   |--------------------|
 *   | region 1           |
 *   |--------------------|
 *   | ...                |
 *   | ...                |
 *   | ...                |
 *   |--------------------|
 *   | region nregs-1     |
 *   \--------------------/
 */
struct arena_bin_info_s {
	/* Size of regions in a slab for this bin's size class. */
	size_t			reg_size;

	/* Total size of a slab for this bin's size class. */
	size_t			slab_size;

	/* Total number of regions in a slab for this bin's size class. */
	uint32_t		nregs;

	/*
	 * Metadata used to manipulate bitmaps for slabs associated with this
	 * bin.
	 */
	bitmap_info_t		bitmap_info;
};

struct arena_decay_s {
	/*
	 * Approximate time in seconds from the creation of a set of unused
	 * dirty pages until an equivalent set of unused dirty pages is purged
	 * and/or reused.
	 */
	ssize_t			time;
	/* time / SMOOTHSTEP_NSTEPS. */
	nstime_t		interval;
	/*
	 * Time at which the current decay interval logically started.  We do
	 * not actually advance to a new epoch until sometime after it starts
	 * because of scheduling and computation delays, and it is even possible
	 * to completely skip epochs.  In all cases, during epoch advancement we
	 * merge all relevant activity into the most recently recorded epoch.
	 */
	nstime_t		epoch;
	/* Deadline randomness generator. */
	uint64_t		jitter_state;
	/*
	 * Deadline for current epoch.  This is the sum of interval and per
	 * epoch jitter which is a uniform random variable in [0..interval).
	 * Epochs always advance by precise multiples of interval, but we
	 * randomize the deadline to reduce the likelihood of arenas purging in
	 * lockstep.
	 */
	nstime_t		deadline;
	/*
	 * Number of dirty pages at beginning of current epoch.  During epoch
	 * advancement we use the delta between arena->decay.ndirty and
	 * arena->ndirty to determine how many dirty pages, if any, were
	 * generated.
	 */
	size_t			nunpurged;
	/*
	 * Trailing log of how many unused dirty pages were generated during
	 * each of the past SMOOTHSTEP_NSTEPS decay epochs, where the last
	 * element is the most recent epoch.  Corresponding epoch times are
	 * relative to epoch.
	 */
	size_t			backlog[SMOOTHSTEP_NSTEPS];
};

struct arena_bin_s {
	/* All operations on arena_bin_t fields require lock ownership. */
	malloc_mutex_t		lock;

	/*
	 * Current slab being used to service allocations of this bin's size
	 * class.  slabcur is independent of slabs_{nonfull,full}; whenever
	 * slabcur is reassigned, the previous slab must be deallocated or
	 * inserted into slabs_{nonfull,full}.
	 */
	extent_t		*slabcur;

	/*
	 * Heap of non-full slabs.  This heap is used to assure that new
	 * allocations come from the non-full slab that is oldest/lowest in
	 * memory.
	 */
	extent_heap_t		slabs_nonfull;

	/* Ring sentinel used to track full slabs. */
	extent_t		slabs_full;

	/* Bin statistics. */
	malloc_bin_stats_t	stats;
};

struct arena_s {
	/*
	 * Number of threads currently assigned to this arena, synchronized via
	 * atomic operations.  Each thread has two distinct assignments, one for
	 * application-serving allocation, and the other for internal metadata
	 * allocation.  Internal metadata must not be allocated from arenas
	 * explicitly created via the arenas.create mallctl, because the
	 * arena.<i>.reset mallctl indiscriminately discards all allocations for
	 * the affected arena.
	 *
	 *   0: Application allocation.
	 *   1: Internal metadata allocation.
	 */
	unsigned		nthreads[2];

	/*
	 * There are three classes of arena operations from a locking
	 * perspective:
	 * 1) Thread assignment (modifies nthreads) is synchronized via atomics.
	 * 2) Bin-related operations are protected by bin locks.
	 * 3) Extent-related operations are protected by this mutex.
	 */
	malloc_mutex_t		lock;

	arena_stats_t		stats;
	/*
	 * List of tcaches for extant threads associated with this arena.
	 * Stats from these are merged incrementally, and at exit if
	 * opt_stats_print is enabled.
	 */
	ql_head(tcache_t)	tcache_ql;

	uint64_t		prof_accumbytes;

	/*
	 * PRNG state for cache index randomization of large allocation base
	 * pointers.
	 */
	size_t			offset_state;

	/* Extent serial number generator state. */
	size_t			extent_sn_next;

	dss_prec_t		dss_prec;

	/* True if a thread is currently executing arena_purge_to_limit(). */
	bool			purging;

	/* Number of pages in active extents. */
	size_t			nactive;

	/*
	 * Current count of pages within unused extents that are potentially
	 * dirty, and for which pages_purge_*() has not been called.  By
	 * tracking this, we can institute a limit on how much dirty unused
	 * memory is mapped for each arena.
	 */
	size_t			ndirty;

	/* Decay-based purging state. */
	arena_decay_t		decay;

	/* Extant large allocations. */
	ql_head(extent_t)	large;
	/* Synchronizes all large allocation/update/deallocation. */
	malloc_mutex_t		large_mtx;

	/*
	 * Heaps of extents that were previously allocated.  These are used when
	 * allocating extents, in an attempt to re-use address space.
	 */
	extent_heap_t		extents_cached[NPSIZES+1];
	extent_heap_t		extents_retained[NPSIZES+1];
	/*
	 * Ring sentinel used to track unused dirty memory.  Dirty memory is
	 * managed as an LRU of cached extents.
	 */
	extent_t		extents_dirty;
	/* Protects extents_{cached,retained,dirty}. */
	malloc_mutex_t		extents_mtx;

	/*
	 * Next extent size class in a growing series to use when satisfying a
	 * request via the extent hooks (only if !config_munmap).  This limits
	 * the number of disjoint virtual memory ranges so that extent merging
	 * can be effective even if multiple arenas' extent allocation requests
	 * are highly interleaved.
	 */
	pszind_t		extent_grow_next;

	/* Cache of extent structures that were allocated via base_alloc(). */
	ql_head(extent_t)	extent_cache;
	malloc_mutex_t		extent_cache_mtx;

	/* bins is used to store heaps of free regions. */
	arena_bin_t		bins[NBINS];

	/* Base allocator, from which arena metadata are allocated. */
	base_t			*base;
};

/* Used in conjunction with tsd for fast arena-related context lookup. */
struct arena_tdata_s {
	ticker_t		decay_ticker;
};

#endif /* JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H */
