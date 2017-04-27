#ifndef JEMALLOC_INTERNAL_MUTEX_INLINES_H
#define JEMALLOC_INTERNAL_MUTEX_INLINES_H

#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/tsd_types.h"

void	malloc_mutex_lock_slow(malloc_mutex_t *mutex);

static inline void
malloc_mutex_lock_final(malloc_mutex_t *mutex) {
	MALLOC_MUTEX_LOCK(mutex);
}

static inline bool
malloc_mutex_trylock_final(malloc_mutex_t *mutex) {
	return MALLOC_MUTEX_TRYLOCK(mutex);
}

static inline void
mutex_owner_stats_update(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	if (config_stats) {
		mutex_prof_data_t *data = &mutex->prof_data;
		data->n_lock_ops++;
		if (data->prev_owner != tsdn) {
			data->prev_owner = tsdn;
			data->n_owner_switches++;
		}
	}
}

/* Trylock: return false if the lock is successfully acquired. */
static inline bool
malloc_mutex_trylock(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_not_owner(tsdn, &mutex->witness);
	if (isthreaded) {
		if (malloc_mutex_trylock_final(mutex)) {
			return true;
		}
		mutex_owner_stats_update(tsdn, mutex);
	}
	witness_lock(tsdn, &mutex->witness);

	return false;
}

/* Aggregate lock prof data. */
static inline void
malloc_mutex_prof_merge(mutex_prof_data_t *sum, mutex_prof_data_t *data) {
	nstime_add(&sum->tot_wait_time, &data->tot_wait_time);
	if (nstime_compare(&sum->max_wait_time, &data->max_wait_time) < 0) {
		nstime_copy(&sum->max_wait_time, &data->max_wait_time);
	}

	sum->n_wait_times += data->n_wait_times;
	sum->n_spin_acquired += data->n_spin_acquired;

	if (sum->max_n_thds < data->max_n_thds) {
		sum->max_n_thds = data->max_n_thds;
	}
	uint32_t cur_n_waiting_thds = atomic_load_u32(&sum->n_waiting_thds,
	    ATOMIC_RELAXED);
	uint32_t new_n_waiting_thds = cur_n_waiting_thds + atomic_load_u32(
	    &data->n_waiting_thds, ATOMIC_RELAXED);
	atomic_store_u32(&sum->n_waiting_thds, new_n_waiting_thds,
	    ATOMIC_RELAXED);
	sum->n_owner_switches += data->n_owner_switches;
	sum->n_lock_ops += data->n_lock_ops;
}

static inline void
malloc_mutex_lock(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_not_owner(tsdn, &mutex->witness);
	if (isthreaded) {
		if (malloc_mutex_trylock_final(mutex)) {
			malloc_mutex_lock_slow(mutex);
		}
		mutex_owner_stats_update(tsdn, mutex);
	}
	witness_lock(tsdn, &mutex->witness);
}

static inline void
malloc_mutex_unlock(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_unlock(tsdn, &mutex->witness);
	if (isthreaded) {
		MALLOC_MUTEX_UNLOCK(mutex);
	}
}

static inline void
malloc_mutex_assert_owner(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_owner(tsdn, &mutex->witness);
}

static inline void
malloc_mutex_assert_not_owner(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	witness_assert_not_owner(tsdn, &mutex->witness);
}

/* Copy the prof data from mutex for processing. */
static inline void
malloc_mutex_prof_read(tsdn_t *tsdn, mutex_prof_data_t *data,
    malloc_mutex_t *mutex) {
	mutex_prof_data_t *source = &mutex->prof_data;
	/* Can only read holding the mutex. */
	malloc_mutex_assert_owner(tsdn, mutex);

	/*
	 * Not *really* allowed (we shouldn't be doing non-atomic loads of
	 * atomic data), but the mutex protection makes this safe, and writing
	 * a member-for-member copy is tedious for this situation.
	 */
	*data = *source;
	/* n_wait_thds is not reported (modified w/o locking). */
	atomic_store_u32(&data->n_waiting_thds, 0, ATOMIC_RELAXED);
}

#endif /* JEMALLOC_INTERNAL_MUTEX_INLINES_H */
