#ifndef JEMALLOC_INTERNAL_MUTEX_STRUCTS_H
#define JEMALLOC_INTERNAL_MUTEX_STRUCTS_H

struct mutex_prof_data_s {
	/*
	 * Counters touched on the slow path, i.e. when there is lock
	 * contention.  We update them once we have the lock.
	 */
	/* Total time (in nano seconds) spent waiting on this mutex. */
	nstime_t		tot_wait_time;
	/* Max time (in nano seconds) spent on a single lock operation. */
	nstime_t		max_wait_time;
	/* # of times have to wait for this mutex (after spinning). */
	uint64_t		n_wait_times;
	/* # of times acquired the mutex through local spinning. */
	uint64_t		n_spin_acquired;
	/* Max # of threads waiting for the mutex at the same time. */
	uint32_t		max_n_thds;
	/* Current # of threads waiting on the lock.  Atomic synced. */
	uint32_t		n_waiting_thds;

	/*
	 * Data touched on the fast path.  These are modified right after we
	 * grab the lock, so it's placed closest to the end (i.e. right before
	 * the lock) so that we have a higher chance of them being on the same
	 * cacheline.
	 */
	/* # of times the mutex holder is different than the previous one. */
	uint64_t		n_owner_switches;
	/* Previous mutex holder, to facilitate n_owner_switches. */
	tsdn_t			*prev_owner;
	/* # of lock() operations in total. */
	uint64_t		n_lock_ops;
};

struct malloc_mutex_s {
	union {
		struct {
			/*
			 * prof_data is defined first to reduce cacheline
			 * bouncing: the data is not touched by the mutex holder
			 * during unlocking, while might be modified by
			 * contenders.  Having it before the mutex itself could
			 * avoid prefetching a modified cacheline (for the
			 * unlocking thread).
			 */
			mutex_prof_data_t	prof_data;
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
			SRWLOCK         	lock;
#  else
			CRITICAL_SECTION	lock;
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
			os_unfair_lock		lock;
#elif (defined(JEMALLOC_OSSPIN))
			OSSpinLock		lock;
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
			pthread_mutex_t		lock;
			malloc_mutex_t		*postponed_next;
#else
			pthread_mutex_t		lock;
#endif
		};
		/*
		 * We only touch witness when configured w/ debug.  However we
		 * keep the field in a union when !debug so that we don't have
		 * to pollute the code base with #ifdefs, while avoid paying the
		 * memory cost.
		 */
#if !defined(JEMALLOC_DEBUG)
		witness_t		witness;
#endif
	};

#if defined(JEMALLOC_DEBUG)
	witness_t		witness;
#endif
};

#endif /* JEMALLOC_INTERNAL_MUTEX_STRUCTS_H */
