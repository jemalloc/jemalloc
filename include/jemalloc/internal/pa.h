#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/lockedint.h"

enum pa_decay_purge_setting_e {
	PA_DECAY_PURGE_ALWAYS,
	PA_DECAY_PURGE_NEVER,
	PA_DECAY_PURGE_ON_EPOCH_ADVANCE
};
typedef enum pa_decay_purge_setting_e pa_decay_purge_setting_t;

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

typedef struct pa_extent_stats_s pa_extent_stats_t;
struct pa_extent_stats_s {
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
	 * Number of unused virtual memory bytes currently retained.  Retained
	 * bytes are technically mapped (though always decommitted or purged),
	 * but they are excluded from the mapped statistic (above).
	 */
	size_t retained; /* Derived. */

	/*
	 * Number of bytes currently mapped, excluding retained memory (and any
	 * base-allocated memory, which is tracked by the arena stats).
	 *
	 * We name this "pa_mapped" to avoid confusion with the arena_stats
	 * "mapped".
	 */
	atomic_zu_t pa_mapped;

	/* Number of edata_t structs allocated by base, but not being used. */
	size_t edata_avail; /* Derived. */

	/* VM space had to be leaked (undocumented).  Normally 0. */
	atomic_zu_t abandoned_vm;
};

/*
 * The local allocator handle.  Keeps the state necessary to satisfy page-sized
 * allocations.
 *
 * The contents are mostly internal to the PA module.  The key exception is that
 * arena decay code is allowed to grab pointers to the dirty and muzzy ecaches
 * decay_ts, for a couple of queries, passing them back to a PA function, or
 * acquiring decay.mtx and looking at decay.purging.  The reasoning is that,
 * while PA decides what and how to purge, the arena code decides when and where
 * (e.g. on what thread).  It's allowed to use the presence of another purger to
 * decide.
 * (The background thread code also touches some other decay internals, but
 * that's not fundamental; its' just an artifact of a partial refactoring, and
 * its accesses could be straightforwardly moved inside the decay module).
 */
typedef struct pa_shard_s pa_shard_t;
struct pa_shard_s {
	/*
	 * Number of pages in active extents.
	 *
	 * Synchronization: atomic.
	 */
	atomic_zu_t nactive;

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
	 * Synchronization: via the internal mutex.
	 */
	decay_t decay_dirty; /* dirty --> muzzy */
	decay_t decay_muzzy; /* muzzy --> retained */

	/* The base from which we get the ehooks and allocate metadat. */
	base_t *base;
};

static inline ssize_t
pa_shard_dirty_decay_ms_get(pa_shard_t *shard) {
	return decay_ms_read(&shard->decay_dirty);
}
static inline ssize_t
pa_shard_muzzy_decay_ms_get(pa_shard_t *shard) {
	return decay_ms_read(&shard->decay_muzzy);
}

static inline bool
pa_shard_dont_decay_muzzy(pa_shard_t *shard) {
	return ecache_npages_get(&shard->ecache_muzzy) == 0 &&
	    pa_shard_muzzy_decay_ms_get(shard) <= 0;
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
/*
 * This does the PA-specific parts of arena reset (i.e. freeing all active
 * allocations).
 */
void pa_shard_reset(pa_shard_t *shard);
/*
 * Destroy all the remaining retained extents.  Should only be called after
 * decaying all active, dirty, and muzzy extents to the retained state, as the
 * last step in destroying the shard.
 */
void pa_shard_destroy_retained(tsdn_t *tsdn, pa_shard_t *shard);

size_t pa_shard_extent_sn_next(pa_shard_t *shard);

/* Gets an edata for the given allocation. */
edata_t *pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size,
    size_t alignment, bool slab, szind_t szind, bool *zero);
/* Returns true on error, in which case nothing changed. */
bool pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool slab, bool *zero);
/*
 * The same.  Sets *generated_dirty to true if we produced new dirty pages, and
 * false otherwise.
 */
bool pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool slab, bool *generated_dirty);
/*
 * Frees the given edata back to the pa.  Sets *generated_dirty if we produced
 * new dirty pages (well, we alwyas set it for now; but this need not be the
 * case).
 * (We could make generated_dirty the return value of course, but this is more
 * consistent with the shrink pathway and our error codes here).
 */
void pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *generated_dirty);

/*
 * All purging functions require holding decay->mtx.  This is one of the few
 * places external modules are allowed to peek inside pa_shard_t internals.
 */

/*
 * Decays the number of pages currently in the ecache.  This might not leave the
 * ecache empty if other threads are inserting dirty objects into it
 * concurrently with the call.
 */
void pa_decay_all(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay);
/*
 * Updates decay settings for the current time, and conditionally purges in
 * response (depending on decay_purge_setting).  Returns whether or not the
 * epoch advanced.
 */
bool pa_maybe_decay_purge(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pa_shard_decay_stats_t *decay_stats, ecache_t *ecache,
    pa_decay_purge_setting_t decay_purge_setting);

/******************************************************************************/
/*
 * Various bits of "boring" functionality that are still part of this module,
 * but that we relegate to pa_extra.c, to keep the core logic in pa.c as
 * readable as possible.
 */

/*
 * These fork phases are synchronized with the arena fork phase numbering to
 * make it easy to keep straight. That's why there's no prefork1.
 */
void pa_shard_prefork0(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork2(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork3(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_prefork4(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_postfork_parent(tsdn_t *tsdn, pa_shard_t *shard);
void pa_shard_postfork_child(tsdn_t *tsdn, pa_shard_t *shard);

void pa_shard_basic_stats_merge(pa_shard_t *shard, size_t *nactive,
    size_t *ndirty, size_t *nmuzzy);

static inline size_t
pa_shard_pa_mapped(pa_shard_t *shard) {
	return atomic_load_zu(&shard->stats->pa_mapped, ATOMIC_RELAXED);
}

void pa_shard_stats_merge(tsdn_t *tsdn, pa_shard_t *shard,
    pa_shard_stats_t *shard_stats_out, pa_extent_stats_t *extent_stats_out,
    size_t *resident);

/*
 * Reads the PA-owned mutex stats into the output stats array, at the
 * appropriate positions.  Morally, these stats should really live in
 * pa_shard_stats_t, but the indices are sort of baked into the various mutex
 * prof macros.  This would be a good thing to do at some point.
 */
void pa_shard_mtx_stats_read(tsdn_t *tsdn, pa_shard_t *shard,
    mutex_prof_data_t mutex_prof_data[mutex_prof_num_arena_mutexes]);

#endif /* JEMALLOC_INTERNAL_PA_H */
