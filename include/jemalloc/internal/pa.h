#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"

/*
 * The page allocator; responsible for acquiring pages of memory for
 * allocations.
 */

typedef struct pa_shard_stats_s pa_shard_stats_t;
struct pa_shard_stats_s {
	/* VM space had to be leaked (undocumented).  Normally 0. */
	atomic_zu_t abandoned_vm;
};

typedef struct pa_shard_s pa_shard_t;
struct pa_shard_s {
	/*
	 * Collections of extents that were previously allocated.  These are
	 * used when allocating extents, in an attempt to re-use address space.
	 *
	 * Synchronization: internal.
	 */
	ecache_t ecache_dirty;
	ecache_t ecache_muzzy;
	ecache_t ecache_retained;

	/* The source of edata_t objects. */
	edata_cache_t edata_cache;

	/* The grow info for the retained ecache. */
	ecache_grow_t ecache_grow;

	pa_shard_stats_t *stats;
};

/* Returns true on error. */
bool pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, base_t *base, unsigned ind,
    pa_shard_stats_t *stats);

#endif /* JEMALLOC_INTERNAL_PA_H */
