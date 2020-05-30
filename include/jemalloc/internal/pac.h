#ifndef JEMALLOC_INTERNAL_PAC_H
#define JEMALLOC_INTERNAL_PAC_H

/*
 * Page allocator classic; an implementation of the PAI interface that:
 * - Can be used for arenas with custom extent hooks.
 * - Can always satisfy any allocation request (including highly-fragmentary
 *   ones).
 * - Can use efficient OS-level zeroing primitives for demand-filled pages.
 */

typedef struct pac_s pac_t;
struct pac_s {
	/*
	 * Collections of extents that were previously allocated.  These are
	 * used when allocating extents, in an attempt to re-use address space.
	 *
	 * Synchronization: internal.
	 */
	ecache_t ecache_dirty;
	ecache_t ecache_muzzy;
	ecache_t ecache_retained;

	edata_cache_t *edata_cache;
};

#endif /* JEMALLOC_INTERNAL_PAC_H */
