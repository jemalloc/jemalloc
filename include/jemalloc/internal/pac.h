#ifndef JEMALLOC_INTERNAL_PAC_H
#define JEMALLOC_INTERNAL_PAC_H

/*
 * Page allocator classic; an implementation of the PAI interface that:
 * - Can be used for arenas with custom extent hooks.
 * - Can always satisfy any allocation request (including highly-fragmentary
 *   ones).
 * - Can use efficient OS-level zeroing primitives for demand-filled pages.
 */

typedef struct pac_decay_stats_s pac_decay_stats_t;
struct pac_decay_stats_s {
	/* Total number of purge sweeps. */
	locked_u64_t npurge;
	/* Total number of madvise calls made. */
	locked_u64_t nmadvise;
	/* Total number of pages purged. */
	locked_u64_t purged;
};

typedef struct pac_estats_s pac_estats_t;
struct pac_estats_s {
	/*
	 * Stats for a given index in the range [0, SC_NPSIZES] in the various
	 * ecache_ts.
	 * We track both bytes and # of extents: two extents in the same bucket
	 * may have different sizes if adjacent size classes differ by more than
	 * a page, so bytes cannot always be derived from # of extents.
	 */
	size_t ndirty;
	size_t dirty_bytes;
	size_t nmuzzy;
	size_t muzzy_bytes;
	size_t nretained;
	size_t retained_bytes;
};

typedef struct pac_stats_s pac_stats_t;
struct pac_stats_s {
	pac_decay_stats_t decay_dirty;
	pac_decay_stats_t decay_muzzy;

	/*
	 * Number of unused virtual memory bytes currently retained.  Retained
	 * bytes are technically mapped (though always decommitted or purged),
	 * but they are excluded from the mapped statistic (above).
	 */
	size_t retained; /* Derived. */

	/*
	 * Number of bytes currently mapped, excluding retained memory (and any
	 * base-allocated memory, which is tracked by the arena stats).
	 *
	 * We name this "pac_mapped" to avoid confusion with the arena_stats
	 * "mapped".
	 */
	atomic_zu_t pac_mapped;

	/* VM space had to be leaked (undocumented).  Normally 0. */
	atomic_zu_t abandoned_vm;
};

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

	malloc_mutex_t *stats_mtx;
	pac_stats_t *stats;

	/* Extent serial number generator state. */
	atomic_zu_t extent_sn_next;
};

bool pac_init(tsdn_t *tsdn, pac_t *pac, unsigned ind, emap_t *emap,
    edata_cache_t *edata_cache, nstime_t *cur_time, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms, pac_stats_t *pac_stats, malloc_mutex_t *stats_mtx);
bool pac_retain_grow_limit_get_set(tsdn_t *tsdn, pac_t *pac, size_t *old_limit,
    size_t *new_limit);
void pac_stats_merge(tsdn_t *tsdn, pac_t *pac, pac_stats_t *pac_stats_out,
    pac_estats_t *estats_out, size_t *resident);

static inline ssize_t
pac_dirty_decay_ms_get(pac_t *pac) {
	return decay_ms_read(&pac->decay_dirty);
}

static inline ssize_t
pac_muzzy_decay_ms_get(pac_t *pac) {
	return decay_ms_read(&pac->decay_muzzy);
}

static inline size_t
pac_mapped(pac_t *pac) {
	return atomic_load_zu(&pac->stats->pac_mapped, ATOMIC_RELAXED);
}

#endif /* JEMALLOC_INTERNAL_PAC_H */
