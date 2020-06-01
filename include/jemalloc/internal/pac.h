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

	emap_t *emap;
	edata_cache_t *edata_cache;

	/* The grow info for the retained ecache. */
	ecache_grow_t ecache_grow;

	/*
	 * Decay-based purging state, responsible for scheduling extent state
	 * transitions.
	 *
	 * Synchronization: via the internal mutex.
	 */
	decay_t decay_dirty; /* dirty --> muzzy */
	decay_t decay_muzzy; /* muzzy --> retained */
};

bool pac_init(tsdn_t *tsdn, pac_t *pac, unsigned ind, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms);
bool pac_retain_grow_limit_get_set(tsdn_t *tsdn, pac_t *pac, size_t *old_limit,
    size_t *new_limit);

static inline ssize_t
pac_dirty_decay_ms_get(pac_t *pac) {
	return decay_ms_read(&pac->decay_dirty);
}

static inline ssize_t
pac_muzzy_decay_ms_get(pac_t *pac) {
	return decay_ms_read(&pac->decay_muzzy);
}

#endif /* JEMALLOC_INTERNAL_PAC_H */
