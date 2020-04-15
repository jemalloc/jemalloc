#define JEMALLOC_COUNTER_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/counter.h"

bool
counter_accum_init(counter_accum_t *counter, uint64_t interval) {
#ifndef JEMALLOC_ATOMIC_U64
	if (malloc_mutex_init(&counter->mtx, "counter_accum",
	    WITNESS_RANK_COUNTER_ACCUM, malloc_mutex_rank_exclusive)) {
		return true;
	}
	counter->accumbytes = 0;
#else
	atomic_store_u64(&counter->accumbytes, 0,
	    ATOMIC_RELAXED);
#endif
	counter->interval = interval;

	return false;
}

void
counter_prefork(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_PREFORK(tsdn, counter->mtx);
}

void
counter_postfork_parent(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_POSTFORK_PARENT(tsdn, counter->mtx);
}

void
counter_postfork_child(tsdn_t *tsdn, counter_accum_t *counter) {
	LOCKEDINT_MTX_POSTFORK_CHILD(tsdn, counter->mtx);
}
