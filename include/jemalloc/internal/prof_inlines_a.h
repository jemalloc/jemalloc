#ifndef JEMALLOC_INTERNAL_PROF_INLINES_A_H
#define JEMALLOC_INTERNAL_PROF_INLINES_A_H

#include "jemalloc/internal/mutex.h"

JEMALLOC_ALWAYS_INLINE void
prof_active_assert() {
	cassert(config_prof);
	/*
	 * If opt_prof is off, then prof_active must always be off, regardless
	 * of whether prof_active_mtx is in effect or not.
	 */
	assert(opt_prof || !prof_active);
}

JEMALLOC_ALWAYS_INLINE bool
prof_active_get_unlocked(void) {
	prof_active_assert();
	/*
	 * Even if opt_prof is true, sampling can be temporarily disabled by
	 * setting prof_active to false.  No locking is used when reading
	 * prof_active in the fast path, so there are no guarantees regarding
	 * how long it will take for all threads to notice state changes.
	 */
	return prof_active;
}

JEMALLOC_ALWAYS_INLINE bool
prof_idump_accum(tsdn_t *tsdn, uint64_t accumbytes) {
	cassert(config_prof);

	if (prof_interval == 0 || !prof_active_get_unlocked()) {
		return false;
	}

	return prof_idump_accum_impl(tsdn, accumbytes);
}

JEMALLOC_ALWAYS_INLINE void
prof_idump_rollback(tsdn_t *tsdn, size_t usize) {
	cassert(config_prof);

	if (prof_interval == 0 || !prof_active_get_unlocked()) {
		return;
	}

	prof_idump_rollback_impl(tsdn, usize);
}

#endif /* JEMALLOC_INTERNAL_PROF_INLINES_A_H */
