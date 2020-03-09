#ifndef JEMALLOC_INTERNAL_DECAY_H
#define JEMALLOC_INTERNAL_DECAY_H

#include "jemalloc/internal/smoothstep.h"

/*
 * The decay_t computes the number of pages we should purge at any given time.
 * Page allocators inform a decay object when pages enter a decay-able state
 * (i.e. dirty or muzzy), and query it to determine how many pages should be
 * purged at any given time.
 */
typedef struct decay_s decay_t;
struct decay_s {
	/* Synchronizes all non-atomic fields. */
	malloc_mutex_t mtx;
	/*
	 * True if a thread is currently purging the extents associated with
	 * this decay structure.
	 */
	bool purging;
	/*
	 * Approximate time in milliseconds from the creation of a set of unused
	 * dirty pages until an equivalent set of unused dirty pages is purged
	 * and/or reused.
	 */
	atomic_zd_t time_ms;
	/* time / SMOOTHSTEP_NSTEPS. */
	nstime_t interval;
	/*
	 * Time at which the current decay interval logically started.  We do
	 * not actually advance to a new epoch until sometime after it starts
	 * because of scheduling and computation delays, and it is even possible
	 * to completely skip epochs.  In all cases, during epoch advancement we
	 * merge all relevant activity into the most recently recorded epoch.
	 */
	nstime_t epoch;
	/* Deadline randomness generator. */
	uint64_t jitter_state;
	/*
	 * Deadline for current epoch.  This is the sum of interval and per
	 * epoch jitter which is a uniform random variable in [0..interval).
	 * Epochs always advance by precise multiples of interval, but we
	 * randomize the deadline to reduce the likelihood of arenas purging in
	 * lockstep.
	 */
	nstime_t deadline;
	/*
	 * Number of unpurged pages at beginning of current epoch.  During epoch
	 * advancement we use the delta between arena->decay_*.nunpurged and
	 * ecache_npages_get(&arena->ecache_*) to determine how many dirty pages,
	 * if any, were generated.
	 */
	size_t nunpurged;
	/*
	 * Trailing log of how many unused dirty pages were generated during
	 * each of the past SMOOTHSTEP_NSTEPS decay epochs, where the last
	 * element is the most recent epoch.  Corresponding epoch times are
	 * relative to epoch.
	 */
	size_t backlog[SMOOTHSTEP_NSTEPS];

	/* Peak number of pages in associated extents.  Used for debug only. */
	uint64_t ceil_npages;
};

#endif /* JEMALLOC_INTERNAL_DECAY_H */
