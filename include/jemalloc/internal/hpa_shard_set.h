#ifndef JEMALLOC_INTERNAL_HPA_SHARD_SET_H
#define JEMALLOC_INTERNAL_HPA_SHARD_SET_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/psset.h"
#include "jemalloc/internal/sz.h"

/*
 * HPA shard sets.
 *
 * Allocation maps the allocation size to a shard set, then picks shard
 * affinity % nshards of that set, where callers pass the requesting arena's
 * index as the affinity.  Every pageslab records the shard that owns it, so
 * deallocation returns an extent to that shard without consulting the sets.
 *
 * The allocation sizes are divided into fixed bands, and the configuration
 * gives each band a shard count.  A band with shards starts a shard set of its
 * own; a band without joins the set of the band in front of it.
 */

/* The number of allocation-size bands, and so the most shard sets. */
#define HPA_SHARD_SET_NBANDS 9

/*
 * The inclusive usable-size upper bound of each band: 4K to 512K by powers of
 * two, then HUGEPAGE, the largest slab_max_alloc.
 */
extern const size_t hpa_shard_set_band_max[HPA_SHARD_SET_NBANDS];

/*
 * The most shards one shard set may have: one per possible arena index.
 * Picking by arena index, a set this large gives every arena its own shard, as
 * when each arena embedded one, instead of making arenas share shards once
 * their indices pass the shard count.  Shards are created lazily, so a shard
 * not created yet costs only its pointer.  This is to be changed in the future
 * because we likely do not need that many shards per shard set.  Setting so
 * for now to ease the migration from per-arena shard to global shard sets
 * in production.
 */
#define HPA_MAX_SHARDS_PER_SET MALLOCX_ARENA_LIMIT

typedef struct hpa_shard_set_s hpa_shard_set_t;
typedef struct hpa_shard_sets_s hpa_shard_sets_t;
struct hpa_shard_set_s {
	/* Inclusive allocation-size upper bound. */
	size_t size_max;
	/* Number of shards. */
	unsigned nshards;
	/*
	 * nshards shard pointers, allocated from the base at boot.  Each starts
	 * NULL; its shard is created when it is first picked.
	 */
	atomic_p_t *shards;
	hpa_shard_sets_t *sets;

	/* Options each shard in this set is initialized with. */
	hpa_shard_opts_t opts;
	sec_opts_t sec_opts;

	/* Serializes the creation of shards. */
	malloc_mutex_t init_mtx;
};

/* Configuration used to construct the shard sets. */
typedef struct hpa_shard_sets_config_s hpa_shard_sets_config_t;
struct hpa_shard_sets_config_s {
	/*
	 * The number of shards in the set each band starts, or 0 for a band
	 * that joins the set of the band below it.  nshards[0] must not be 0.
	 */
	unsigned nshards[HPA_SHARD_SET_NBANDS];
};

/*
 * Options for size-routed HPA shard sets.  opt_hpa_shard_sets selects the
 * layout configured by opt_hpa_shard_sets_nshards, a shard count per band;
 * otherwise every arena has its own hpa shard.
 */
extern bool                    opt_hpa_shard_sets;
extern hpa_shard_sets_config_t opt_hpa_shard_sets_nshards;

struct hpa_shard_sets_s {
	bool initialized;
	unsigned nsets;

	/* Shared inputs retained for lazy shard initialization. */
	base_t *base;
	hpa_central_t *central;
	emap_t *emap;
	edata_cache_t *edata_cache;

	hpa_shard_set_t sets[HPA_SHARD_SET_NBANDS];
};

/*
 * The process-wide shard sets, which malloc_init boots if hpa_shard_sets is
 * on.  They have no sets until booted.
 */
extern hpa_shard_sets_t hpa_shard_sets_global;

bool hpa_shard_sets_config_validate(
    const hpa_shard_sets_config_t *config, size_t slab_max_alloc);

/*
 * Build the shard sets.  Returns true on error, without printing, leaving no
 * sets for the fork hooks to visit.  The sets cover the usable sizes the HPA
 * serves: every small size, and large sizes whose extents, with sz_large_pad,
 * fit in opts->slab_max_alloc.  Bands above those get no set, and the last
 * set ends there.  Shard pointer arrays and lazily created shards are
 * allocated from base, and every shard shares central, emap and edata_cache;
 * since shards are never destroyed, all of these must outlive the sets.
 * Every set's shards are created with opts and sec_opts.
 */
bool hpa_shard_sets_boot(tsdn_t *tsdn, hpa_shard_sets_t *sets, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_shard_sets_config_t *config, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts);

/*
 * Returns the shard set serving allocations of usable size usize, or NULL if
 * usize is above every set's bound (the HPA does not serve it).  Routing uses
 * the allocation size, not the size of the extent that backs it.
 */
hpa_shard_set_t *hpa_shard_sets_lookup(hpa_shard_sets_t *sets, size_t usize);

/*
 * Returns shard affinity % nshards of the set, creating it if it has not
 * been created yet; NULL if that fails.  affinity is a caller-chosen key
 * that keeps related requests on the same shard, such as the requesting
 * arena's index.  As with arena_choose(), the inline part returns a shard
 * already created and leaves creating one to hpa_shard_choose_hard().
 */
hpa_shard_t *hpa_shard_choose_hard(
    tsdn_t *tsdn, hpa_shard_set_t *shard_set, unsigned affinity);

JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_shard_choose(tsdn_t *tsdn, hpa_shard_set_t *shard_set, unsigned affinity) {
	assert(shard_set->nshards > 0);

	unsigned     idx = affinity % shard_set->nshards;
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &shard_set->shards[idx], ATOMIC_ACQUIRE);
	if (shard != NULL) {
		return shard;
	}
	return hpa_shard_choose_hard(tsdn, shard_set, affinity);
}

/*
 * Fork hooks, called in the same numbered stages as the arena fork hooks so
 * lock ranks interleave correctly.  prefork2 takes every set's init_mtx before
 * locking any shard, which freezes the set of initialized shards for the rest
 * of the fork.  The base, central, emap and edata_cache passed to boot are
 * the caller's to fork.  In the child, which has no background threads, the
 * shards created later do not defer their work, like the shard of a manual
 * arena created in the child; the shards that existed at the fork keep their
 * deferral_allowed, as the manual arenas' shards do.
 */
void hpa_shard_sets_prefork2(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_prefork3(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_prefork4(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_postfork_parent(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_postfork_child(tsdn_t *tsdn, hpa_shard_sets_t *sets);

/*
 * The maintenance an arena does on a shard of its own, done on every shard
 * of the sets.  set_deferral_allowed also sets it for the shards created
 * later, and time_until_deferred_work returns the soonest of the shards'.
 */
void hpa_shard_sets_set_deferral_allowed(
    tsdn_t *tsdn, hpa_shard_sets_t *sets, bool deferral_allowed);
void     hpa_shard_sets_do_deferred_work(tsdn_t *tsdn, hpa_shard_sets_t *sets);
uint64_t hpa_shard_sets_time_until_deferred_work(
    tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_flush(tsdn_t *tsdn, hpa_shard_sets_t *sets);

/*
 * The two composed: the whole of the allocation-side routing decision.  See
 * hpa_shard_choose() for affinity.  Returns NULL if no set serves usize or
 * the chosen shard cannot be initialized; the caller then falls back to the
 * PAC.
 */
JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_route(
    tsdn_t *tsdn, hpa_shard_sets_t *sets, size_t usize, unsigned affinity) {
	hpa_shard_set_t *shard_set = hpa_shard_sets_lookup(sets, usize);
	if (shard_set == NULL) {
		return NULL;
	}
	return hpa_shard_choose(tsdn, shard_set, affinity);
}

#endif /* JEMALLOC_INTERNAL_HPA_SHARD_SET_H */
