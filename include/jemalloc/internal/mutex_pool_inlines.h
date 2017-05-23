#ifndef JEMALLOC_INTERNAL_MUTEX_POOL_INLINES_H
#define JEMALLOC_INTERNAL_MUTEX_POOL_INLINES_H

#include "jemalloc/internal/hash.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_pool_structs.h"
#include "jemalloc/internal/witness.h"

/*
 * This file really combines "inlines" and "externs", but only transitionally.
 */

bool mutex_pool_init(mutex_pool_t *pool, const char *name, witness_rank_t rank);

static inline malloc_mutex_t *
mutex_pool_mutex(mutex_pool_t *pool, uintptr_t key) {
	size_t hash_result[2];
	hash(&key, sizeof(key), 0xd50dcc1b, hash_result);
	return &pool->mutexes[hash_result[0] % MUTEX_POOL_SIZE];
}

static inline void
mutex_pool_assert_not_held(tsdn_t *tsdn, mutex_pool_t *pool) {
	for (int i = 0; i < MUTEX_POOL_SIZE; i++) {
		malloc_mutex_assert_not_owner(tsdn, &pool->mutexes[i]);
	}
}

/*
 * Note that a mutex pool doesn't work exactly the way an embdedded mutex would.
 * You're not allowed to acquire mutexes in the pool one at a time.  You have to
 * acquire all the mutexes you'll need in a single function call, and then
 * release them all in a single function call.
 */

static inline void
mutex_pool_lock(tsdn_t *tsdn, mutex_pool_t *pool, uintptr_t key) {
	mutex_pool_assert_not_held(tsdn, pool);

	malloc_mutex_t *mutex = mutex_pool_mutex(pool, key);
	malloc_mutex_lock(tsdn, mutex);
}

static inline void
mutex_pool_unlock(tsdn_t *tsdn, mutex_pool_t *pool, uintptr_t key) {
	malloc_mutex_t *mutex = mutex_pool_mutex(pool, key);
	malloc_mutex_unlock(tsdn, mutex);

	mutex_pool_assert_not_held(tsdn, pool);
}

static inline void
mutex_pool_lock2(tsdn_t *tsdn, mutex_pool_t *pool, uintptr_t key1,
    uintptr_t key2) {
	mutex_pool_assert_not_held(tsdn, pool);

	malloc_mutex_t *mutex1 = mutex_pool_mutex(pool, key1);
	malloc_mutex_t *mutex2 = mutex_pool_mutex(pool, key2);
	if ((uintptr_t)mutex1 < (uintptr_t)mutex2) {
		malloc_mutex_lock(tsdn, mutex1);
		malloc_mutex_lock(tsdn, mutex2);
	} else if ((uintptr_t)mutex1 == (uintptr_t)mutex2) {
		malloc_mutex_lock(tsdn, mutex1);
	} else {
		malloc_mutex_lock(tsdn, mutex2);
		malloc_mutex_lock(tsdn, mutex1);
	}
}

static inline void
mutex_pool_unlock2(tsdn_t *tsdn, mutex_pool_t *pool, uintptr_t key1,
    uintptr_t key2) {
	malloc_mutex_t *mutex1 = mutex_pool_mutex(pool, key1);
	malloc_mutex_t *mutex2 = mutex_pool_mutex(pool, key2);
	if (mutex1 == mutex2) {
		malloc_mutex_unlock(tsdn, mutex1);
	} else {
		malloc_mutex_unlock(tsdn, mutex1);
		malloc_mutex_unlock(tsdn, mutex2);
	}

	mutex_pool_assert_not_held(tsdn, pool);
}

static inline void
mutex_pool_assert_owner(tsdn_t *tsdn, mutex_pool_t *pool, uintptr_t key) {
	malloc_mutex_assert_owner(tsdn, mutex_pool_mutex(pool, key));
}

#endif /* JEMALLOC_INTERNAL_MUTEX_POOL_INLINES_H */
