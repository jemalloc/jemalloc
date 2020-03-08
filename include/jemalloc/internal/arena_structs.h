#ifndef JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H
#define JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H

#include "jemalloc/internal/arena_stats.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/counter.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/pa.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/smoothstep.h"
#include "jemalloc/internal/ticker.h"

struct arena_decay_s {
	/* Synchronizes all non-atomic fields. */
	malloc_mutex_t		mtx;
	/*
	 * True if a thread is currently purging the extents associated with
	 * this decay structure.
	 */
	bool			purging;
	/*
	 * Approximate time in milliseconds from the creation of a set of unused
	 * dirty pages until an equivalent set of unused dirty pages is purged
	 * and/or reused.
	 */
	atomic_zd_t		time_ms;
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
	 * Number of unpurged pages at beginning of current epoch.  During epoch
	 * advancement we use the delta between arena->decay_*.nunpurged and
	 * ecache_npages_get(&arena->ecache_*) to determine how many dirty pages,
	 * if any, were generated.
	 */
	size_t			nunpurged;
	/*
	 * Trailing log of how many unused dirty pages were generated during
	 * each of the past SMOOTHSTEP_NSTEPS decay epochs, where the last
	 * element is the most recent epoch.  Corresponding epoch times are
	 * relative to epoch.
	 */
	size_t			backlog[SMOOTHSTEP_NSTEPS];

	/*
	 * Pointer to associated stats.  These stats are embedded directly in
	 * the arena's stats due to how stats structures are shared between the
	 * arena and ctl code.
	 *
	 * Synchronization: Same as associated arena's stats field. */
	arena_stats_decay_t	*stats;
	/* Peak number of pages in associated extents.  Used for debug only. */
	uint64_t		ceil_npages;
};

struct arena_s {
	/*
	 * Number of threads currently assigned to this arena.  Each thread has
	 * two distinct assignments, one for application-serving allocation, and
	 * the other for internal metadata allocation.  Internal metadata must
	 * not be allocated from arenas explicitly created via the arenas.create
	 * mallctl, because the arena.<i>.reset mallctl indiscriminately
	 * discards all allocations for the affected arena.
	 *
	 *   0: Application allocation.
	 *   1: Internal metadata allocation.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t		nthreads[2];

	/* Next bin shard for binding new threads. Synchronization: atomic. */
	atomic_u_t		binshard_next;

	/*
	 * When percpu_arena is enabled, to amortize the cost of reading /
	 * updating the current CPU id, track the most recent thread accessing
	 * this arena, and only read CPU if there is a mismatch.
	 */
	tsdn_t		*last_thd;

	/* Synchronization: internal. */
	arena_stats_t		stats;

	/*
	 * Lists of tcaches and cache_bin_array_descriptors for extant threads
	 * associated with this arena.  Stats from these are merged
	 * incrementally, and at exit if opt_stats_print is enabled.
	 *
	 * Synchronization: tcache_ql_mtx.
	 */
	ql_head(tcache_t)			tcache_ql;
	ql_head(cache_bin_array_descriptor_t)	cache_bin_array_descriptor_ql;
	malloc_mutex_t				tcache_ql_mtx;

	/* Synchronization: internal. */
	counter_accum_t		prof_accum;

	/*
	 * Extent serial number generator state.
	 *
	 * Synchronization: atomic.
	 */
	atomic_zu_t		extent_sn_next;

	/*
	 * Represents a dss_prec_t, but atomically.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t		dss_prec;

	/*
	 * Number of pages in active extents.
	 *
	 * Synchronization: atomic.
	 */
	atomic_zu_t		nactive;

	/*
	 * Extant large allocations.
	 *
	 * Synchronization: large_mtx.
	 */
	edata_list_t		large;
	/* Synchronizes all large allocation/update/deallocation. */
	malloc_mutex_t		large_mtx;

	/* The page-level allocator shard this arena uses. */
	pa_shard_t		pa_shard;

	/*
	 * Decay-based purging state, responsible for scheduling extent state
	 * transitions.
	 *
	 * Synchronization: internal.
	 */
	arena_decay_t		decay_dirty; /* dirty --> muzzy */
	arena_decay_t		decay_muzzy; /* muzzy --> retained */

	/* The grow info for the retained ecache. */
	ecache_grow_t		ecache_grow;

	/* The source of edata_t objects. */
	edata_cache_t		edata_cache;

	/*
	 * bins is used to store heaps of free regions.
	 *
	 * Synchronization: internal.
	 */
	bins_t			bins[SC_NBINS];

	/*
	 * Base allocator, from which arena metadata are allocated.
	 *
	 * Synchronization: internal.
	 */
	base_t			*base;
	/* Used to determine uptime.  Read-only after initialization. */
	nstime_t		create_time;
};

/* Used in conjunction with tsd for fast arena-related context lookup. */
struct arena_tdata_s {
	ticker_t		decay_ticker;
};

#endif /* JEMALLOC_INTERNAL_ARENA_STRUCTS_B_H */
