#ifndef JEMALLOC_INTERNAL_MUTEX_INLINES_H
#define JEMALLOC_INTERNAL_MUTEX_INLINES_H

void	malloc_mutex_lock_slow(malloc_mutex_t *mutex);

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex);
bool	malloc_mutex_trylock(malloc_mutex_t *mutex);
void	malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_lock_prof_read(tsdn_t *tsdn, lock_prof_data_t *data,
    malloc_mutex_t *mutex);
void	malloc_lock_prof_merge(lock_prof_data_t *sum, lock_prof_data_t *data);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_MUTEX_C_))
JEMALLOC_INLINE void
malloc_mutex_lock_final(malloc_mutex_t *mutex) {
	MALLOC_MUTEX_LOCK(mutex);
}

/* Trylock: return false if the lock is successfully acquired. */
JEMALLOC_INLINE bool
malloc_mutex_trylock(malloc_mutex_t *mutex) {
	return MALLOC_MUTEX_TRYLOCK(mutex);
}

/* Aggregate lock prof data. */
JEMALLOC_INLINE void
malloc_lock_prof_merge(lock_prof_data_t *sum, lock_prof_data_t *data) {
	nstime_add(&sum->tot_wait_time, &data->tot_wait_time);
	if (nstime_compare(&data->max_wait_time, &sum->max_wait_time)) {
		nstime_copy(&sum->max_wait_time, &data->max_wait_time);
	}
	sum->n_wait_times += data->n_wait_times;
	sum->n_spin_acquired += data->n_spin_acquired;

	if (sum->max_n_thds < data->max_n_thds) {
		sum->max_n_thds = data->max_n_thds;
	}
	sum->n_waiting_thds += data->n_waiting_thds;
	sum->n_owner_switches += data->n_owner_switches;
	sum->n_lock_ops += data->n_lock_ops;
}

JEMALLOC_INLINE void
malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_not_owner(tsdn, &mutex->witness);
	if (isthreaded) {
		if (malloc_mutex_trylock(mutex)) {
			malloc_mutex_lock_slow(mutex);
		}
		/* We own the lock now.  Update a few counters. */
		lock_prof_data_t *data = &mutex->prof_data;
		data->n_lock_ops++;
		if (data->prev_owner != tsdn) {
			data->prev_owner = tsdn;
			data->n_owner_switches++;
		}
	}
	witness_lock(tsdn, &mutex->witness);
}

JEMALLOC_INLINE void
malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_unlock(tsdn, &mutex->witness);
	if (isthreaded) {
		MALLOC_MUTEX_UNLOCK(mutex);
	}
}

JEMALLOC_INLINE void
malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_owner(tsdn, &mutex->witness);
}

JEMALLOC_INLINE void
malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_not_owner(tsdn, &mutex->witness);
}

/* Copy the prof data from mutex for processing. */
JEMALLOC_INLINE void
malloc_lock_prof_read(tsdn_t *tsdn, lock_prof_data_t *data,
    malloc_mutex_t *mutex) {
	lock_prof_data_t *source = &mutex->prof_data;
	/* Can only read with the lock. */
	malloc_mutex_assert_owner(tsdn, mutex);

	*data = *source;
	/* n_wait_thds is not reported (modified w/o locking). */
	data->n_waiting_thds = 0;
}

#endif

#endif /* JEMALLOC_INTERNAL_MUTEX_INLINES_H */
