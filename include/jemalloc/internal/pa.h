#ifndef JEMALLOC_INTERNAL_PA_H
#define JEMALLOC_INTERNAL_PA_H

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/decay.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/lockedint.h"
#include "jemalloc/internal/pac.h"
#include "jemalloc/internal/pai.h"

/*
 * The page allocator; responsible for acquiring pages of memory for
 * allocations.  It picks the implementation of the page allocator interface
 * (i.e. a pai_t) to handle a given page-level allocation request.  For now, the
 * only such implementation is the PAC code ("page allocator classic"), but
 * others will be coming soon.
 */

enum pa_decay_purge_setting_e {
	PA_DECAY_PURGE_ALWAYS,
	PA_DECAY_PURGE_NEVER,
	PA_DECAY_PURGE_ON_EPOCH_ADVANCE
};
typedef enum pa_decay_purge_setting_e pa_decay_purge_setting_t;

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
	/* Number of edata_t structs allocated by base, but not being used. */
	size_t edata_avail; /* Derived. */
	/*
	 * Stats specific to the PAC.  For now, these are the only stats that
	 * exist, but there will eventually be other page allocators.  Things
	 * like edata_avail make sense in a cross-PA sense, but things like
	 * npurges don't.
	 */
	pac_stats_t pac_stats;
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
	 * An interface for page allocation from the ecache framework (i.e. a
	 * cascade of ecache_dirty, ecache_muzzy, ecache_retained).  Right now
	 * this is the *only* pai, but we'll soon grow another.
	 */
	pai_t ecache_pai;
	pac_t pac;

	/* The source of edata_t objects. */
	edata_cache_t edata_cache;

	/* Extent serial number generator state. */
	atomic_zu_t extent_sn_next;

	malloc_mutex_t *stats_mtx;
	pa_shard_stats_t *stats;

	/* The emap this shard is tied to. */
	emap_t *emap;

	/* The base from which we get the ehooks and allocate metadat. */
	base_t *base;
};

static inline bool
pa_shard_dont_decay_muzzy(pa_shard_t *shard) {
	return ecache_npages_get(&shard->pac.ecache_muzzy) == 0 &&
	    pac_muzzy_decay_ms_get(&shard->pac) <= 0;
}

static inline bool
pa_shard_may_force_decay(pa_shard_t *shard) {
	return !(pac_dirty_decay_ms_get(&shard->pac) == -1
	    || pac_muzzy_decay_ms_get(&shard->pac) == -1);
}

static inline ehooks_t *
pa_shard_ehooks_get(pa_shard_t *shard) {
	return base_ehooks_get(shard->base);
}

/* Returns true on error. */
bool pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, emap_t *emap, base_t *base,
    unsigned ind, pa_shard_stats_t *stats, malloc_mutex_t *stats_mtx,
    nstime_t *cur_time, ssize_t dirty_decay_ms, ssize_t muzzy_decay_ms);

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
    size_t alignment, bool slab, szind_t szind, bool zero);
/* Returns true on error, in which case nothing changed. */
bool pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool zero);
/*
 * The same.  Sets *generated_dirty to true if we produced new dirty pages, and
 * false otherwise.
 */
bool pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool *generated_dirty);
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
    pac_decay_stats_t *decay_stats, ecache_t *ecache, bool fully_decay);
/*
 * Updates decay settings for the current time, and conditionally purges in
 * response (depending on decay_purge_setting).  Returns whether or not the
 * epoch advanced.
 */
bool pa_maybe_decay_purge(tsdn_t *tsdn, pa_shard_t *shard, decay_t *decay,
    pac_decay_stats_t *decay_stats, ecache_t *ecache,
    pa_decay_purge_setting_t decay_purge_setting);

/*
 * Gets / sets the maximum amount that we'll grow an arena down the
 * grow-retained pathways (unless forced to by an allocaction request).
 *
 * Set new_limit to NULL if it's just a query, or old_limit to NULL if you don't
 * care about the previous value.
 *
 * Returns true on error (if the new limit is not valid).
 */
bool pa_shard_retain_grow_limit_get_set(tsdn_t *tsdn, pa_shard_t *shard,
    size_t *old_limit, size_t *new_limit);

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

void pa_shard_stats_merge(tsdn_t *tsdn, pa_shard_t *shard,
    pa_shard_stats_t *pa_shard_stats_out, pac_estats_t *estats_out,
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
