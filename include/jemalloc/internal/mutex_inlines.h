#ifndef JEMALLOC_INTERNAL_MUTEX_INLINES_H
#define JEMALLOC_INTERNAL_MUTEX_INLINES_H

void	malloc_mutex_lock_slow(malloc_mutex_t *mutex);

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex);
bool	malloc_mutex_trylock(malloc_mutex_t *mutex);
void	malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
void	malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex);
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
#endif

#endif /* JEMALLOC_INTERNAL_MUTEX_INLINES_H */
