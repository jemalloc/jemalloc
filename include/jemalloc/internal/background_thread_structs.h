#ifndef JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H
#define JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H

/* This file really combines "structs" and "types", but only transitionally. */

#define BACKGROUND_THREAD_INDEFINITE_SLEEP UINT64_MAX

struct background_thread_info_s {
#ifdef JEMALLOC_BACKGROUND_THREAD
	/* Background thread is pthread specific. */
	pthread_t		thread;
	pthread_cond_t		cond;
#endif
	malloc_mutex_t		mtx;
	/* Whether the thread has been created. */
	bool			started;
	/* When true, it means no wakeup scheduled. */
	atomic_b_t		indefinite_sleep;
	/* Next scheduled wakeup time (absolute time in ns). */
	nstime_t		next_wakeup;
	/*
	 *  Since the last background thread run, newly added number of pages
	 *  that need to be purged by the next wakeup.  This is adjusted on
	 *  epoch advance, and is used to determine whether we should signal the
	 *  background thread to wake up earlier.
	 */
	size_t			npages_to_purge_new;
	/* Stats: total number of runs since started. */
	uint64_t		tot_n_runs;
	/* Stats: total sleep time since started. */
	nstime_t		tot_sleep_time;
};
typedef struct background_thread_info_s background_thread_info_t;

struct background_thread_stats_s {
	size_t num_threads;
	uint64_t num_runs;
	nstime_t run_interval;
};
typedef struct background_thread_stats_s background_thread_stats_t;

#endif /* JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H */
