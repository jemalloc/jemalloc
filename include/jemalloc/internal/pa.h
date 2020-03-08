#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

/*
 * The page allocator; responsible for acquiring pages of memory for
 * allocations.
 */

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
};

/* Returns true on error. */
bool pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, unsigned ind);

#endif /* JEMALLOC_INTERNAL_PA_H */
