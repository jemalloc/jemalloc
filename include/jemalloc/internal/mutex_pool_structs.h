#ifndef JEMALLOC_INTERNAL_MUTEX_POOL_STRUCTS_H
#define JEMALLOC_INTERNAL_MUTEX_POOL_STRUCTS_H

#include "jemalloc/internal/mutex.h"

/* This file really combines "structs" and "types", but only transitionally. */

/* We do mod reductions by this value, so it should be kept a power of 2. */
#define MUTEX_POOL_SIZE 256

typedef struct mutex_pool_s mutex_pool_t;
struct mutex_pool_s {
	malloc_mutex_t mutexes[MUTEX_POOL_SIZE];
};

#endif /* JEMALLOC_INTERNAL_MUTEX_POOL_STRUCTS_H */
