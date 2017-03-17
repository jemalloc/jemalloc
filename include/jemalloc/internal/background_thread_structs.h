#ifndef JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H
#define JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H

struct background_thread_info_s {
	malloc_mutex_t		mtx;
#ifdef JEMALLOC_BACKGROUND_THREAD
	/* Background thread is pthread specific. */
	pthread_cond_t		cond;
	pthread_t		thread;
	/* Whether the thread has been created. */
	bool			started;
	/* Next scheduled wakeup time (absolute time). */
	nstime_t		next_wakeup;
	/*
	 *  Since the last background thread run, newly added number of pages
	 *  that need to be purged by the next wakeup.  This is adjusted on
	 *  epoch advance, and is used to determine whether we should signal the
	 *  background thread to wake up earlier.
	 */
	size_t			npages_to_purge_new;
#endif /* ifdef JEMALLOC_BACKGROUND_THREAD */
};
typedef struct background_thread_info_s background_thread_info_t;

#endif /* JEMALLOC_INTERNAL_BACKGROUND_THREAD_STRUCTS_H */
