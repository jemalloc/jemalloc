#ifndef JEMALLOC_INTERNAL_EXTENT_INLINES_H
#define JEMALLOC_INTERNAL_EXTENT_INLINES_H

#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_pool.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/prng.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"

static inline void
extent_lock(tsdn_t *tsdn, extent_t *extent) {
	assert(extent != NULL);
	mutex_pool_lock(tsdn, &extent_mutex_pool, (uintptr_t)extent);
}

static inline void
extent_unlock(tsdn_t *tsdn, extent_t *extent) {
	assert(extent != NULL);
	mutex_pool_unlock(tsdn, &extent_mutex_pool, (uintptr_t)extent);
}

static inline void
extent_lock2(tsdn_t *tsdn, extent_t *extent1, extent_t *extent2) {
	assert(extent1 != NULL && extent2 != NULL);
	mutex_pool_lock2(tsdn, &extent_mutex_pool, (uintptr_t)extent1,
	    (uintptr_t)extent2);
}

static inline void
extent_unlock2(tsdn_t *tsdn, extent_t *extent1, extent_t *extent2) {
	assert(extent1 != NULL && extent2 != NULL);
	mutex_pool_unlock2(tsdn, &extent_mutex_pool, (uintptr_t)extent1,
	    (uintptr_t)extent2);
}

#endif /* JEMALLOC_INTERNAL_EXTENT_INLINES_H */
