#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/lockedint.h"

/*
 * The page allocator; responsible for acquiring pages of memory for
 * allocations.
 */

typedef struct pa_shard_decay_stats_s pa_shard_decay_stats_t;
struct pa_shard_decay_stats_s {
	/* Total number of purge sweeps. */
	locked_u64_t npurge;
	/* Total number of madvise calls made. */
	locked_u64_t nmadvise;
	/* Total number of pages purged. */
	locked_u64_t purged;
};

/*
 * The stats for a particular pa_shard.  Because of the way the ctl module
 * handles stats epoch data collection (it has its own arena_stats, and merges
 * the stats from each arena into it), this needs to live in the arena_stats_t;
 * hence we define it here and let the pa_shard have a pointer (rather than the
 * more natural approach of just embedding it in the pa_shard itself).
 *
 * We follow the arena_stats_t approach of marking the derived fields.  These
 * are the ones that are not maintained on their own; instead, their values are
 * derived during those stats merges.
 */
typedef struct pa_shard_stats_s pa_shard_stats_t;
struct pa_shard_stats_s {
	pa_shard_decay_stats_t decay_dirty;
	pa_shard_decay_stats_t decay_muzzy;
	/*
	 * Number of bytes currently mapped, excluding retained memory.
	 *
	 * Partially derived -- we maintain our own counter, but add in the
	 * base's own counter at merge.
	 */
	locked_zu_t mapped;

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

	/* Extent serial number generator state. */
	atomic_zu_t extent_sn_next;

	malloc_mutex_t *stats_mtx;
	pa_shard_stats_t *stats;

	/*
	 * Decay-based purging state, responsible for scheduling extent state
	 * transitions.
	 *
	 * Synchronization: internal.
	 */
	decay_t decay_dirty; /* dirty --> muzzy */
	decay_t decay_muzzy; /* muzzy --> retained */

	/* The base from which we get the ehooks and allocate metadat. */
	base_t *base;
};

static inline void
pa_shard_stats_mapped_add(tsdn_t *tsdn, pa_shard_t *shard, size_t size) {
	LOCKEDINT_MTX_LOCK(tsdn, *shard->stats_mtx);
	locked_inc_zu(tsdn, LOCKEDINT_MTX(*shard->stats_mtx),
	    &shard->stats->mapped, size);
	LOCKEDINT_MTX_UNLOCK(tsdn, *shard->stats_mtx);
}

static inline ssize_t
pa_shard_dirty_decay_ms_get(pa_shard_t *shard) {
	return decay_ms_read(&shard->decay_dirty);
}
static inline ssize_t
pa_shard_muzzy_decay_ms_get(pa_shard_t *shard) {
	return decay_ms_read(&shard->decay_muzzy);
}

static inline bool
pa_shard_may_force_decay(pa_shard_t *shard) {
	return !(pa_shard_dirty_decay_ms_get(shard) == -1
	    || pa_shard_muzzy_decay_ms_get(shard) == -1);
}

static inline ehooks_t *
pa_shard_ehooks_get(pa_shard_t *shard) {
	return base_ehooks_get(shard->base);
}

/* Returns true on error. */
bool pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, base_t *base, unsigned ind,
    pa_shard_stats_t *stats, malloc_mutex_t *stats_mtx);
size_t pa_shard_extent_sn_next(pa_shard_t *shard);

edata_t *pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size,
    size_t alignment, bool slab, szind_t szind, bool *zero, size_t *mapped_add);
/* Returns true on error, in which case nothing changed. */
bool pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    size_t new_usize, bool *zero, size_t *mapped_add);

#endif /* JEMALLOC_INTERNAL_PA_H */
