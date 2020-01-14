#ifndef JEMALLOC_INTERNAL_COUNTER_H
#define JEMALLOC_INTERNAL_COUNTER_H

#include "jemalloc/internal/mutex.h"

typedef struct counter_accum_s {
#ifndef JEMALLOC_ATOMIC_U64
	malloc_mutex_t	mtx;
	uint64_t accumbytes;
#else
	atomic_u64_t accumbytes;
#endif
	uint64_t interval;
} counter_accum_t;

JEMALLOC_ALWAYS_INLINE bool
counter_accum(tsdn_t *tsdn, counter_accum_t *counter, uint64_t accumbytes) {
	bool overflow;
	uint64_t a0, a1;

	/*
	 * If the event moves fast enough (and/or if the event handling is slow
	 * enough), extreme overflow here (a1 >= interval * 2) can cause counter
	 * trigger coalescing.  This is an intentional mechanism that avoids
	 * rate-limiting allocation.
	 */
	uint64_t interval = counter->interval;
	assert(interval > 0);
#ifdef JEMALLOC_ATOMIC_U64
	a0 = atomic_load_u64(&counter->accumbytes, ATOMIC_RELAXED);
	do {
		a1 = a0 + accumbytes;
		assert(a1 >= a0);
		overflow = (a1 >= interval);
		if (overflow) {
			a1 %= interval;
		}
	} while (!atomic_compare_exchange_weak_u64(&counter->accumbytes, &a0, a1,
	    ATOMIC_RELAXED, ATOMIC_RELAXED));
#else
	malloc_mutex_lock(tsdn, &counter->mtx);
	a0 = counter->accumbytes;
	a1 = a0 + accumbytes;
	overflow = (a1 >= interval);
	if (overflow) {
		a1 %= interval;
	}
	counter->accumbytes = a1;
	malloc_mutex_unlock(tsdn, &counter->mtx);
#endif
	return overflow;
}

JEMALLOC_ALWAYS_INLINE void
counter_rollback(tsdn_t *tsdn, counter_accum_t *counter, uint64_t bytes) {
	/*
	 * Cancel out as much of the excessive accumbytes increase as possible
	 * without underflowing.  Interval-triggered events occur slightly more
	 * often than intended as a result of incomplete canceling.
	 */
	uint64_t a0, a1;
#ifdef JEMALLOC_ATOMIC_U64
	a0 = atomic_load_u64(&counter->accumbytes,
	    ATOMIC_RELAXED);
	do {
		a1 = (a0 >= bytes) ? a0 - bytes : 0;
	} while (!atomic_compare_exchange_weak_u64(
	    &counter->accumbytes, &a0, a1, ATOMIC_RELAXED,
	    ATOMIC_RELAXED));
#else
	malloc_mutex_lock(tsdn, &counter->mtx);
	a0 = counter->accumbytes;
	a1 = (a0 >= bytes) ?  a0 - bytes : 0;
	counter->accumbytes = a1;
	malloc_mutex_unlock(tsdn, &counter->mtx);
#endif
}

bool counter_accum_init(counter_accum_t *counter, uint64_t interval);

#endif /* JEMALLOC_INTERNAL_COUNTER_H */
