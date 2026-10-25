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
 * Allocation maps the allocation size to a shard set, then
 * uses the set's picker policy to select a shard.  A shard's owner id, which
 * is recorded on every pageslab it owns, is its slot index within its set, so
 * ids are unique only within a set.  Deallocation routes the extent's
 * allocation size to the set and returns the extent to the shard with that id.
 */

/* How a shard set chooses one of its shard slots. */
typedef enum hpa_shard_set_pick_e {
	/* slot = affinity % nshards; callers pass the requesting arena. */
	hpa_shard_set_pick_arena = 0,
	/* Atomic counter.  Ignores affinity and spreads load across slots. */
	hpa_shard_set_pick_roundrobin = 1,
	hpa_shard_set_pick_limit
} hpa_shard_set_pick_t;

/* Configuration names indexed by hpa_shard_set_pick_t. */
extern const char *const hpa_shard_set_pick_type_names[];

/* A cap on shard sets, purely so the container can be a fixed-size struct. */
#define HPA_MAX_SHARD_SETS 8

/*
 * The most shard slots one shard set may have: one per possible arena index.
 * With the arena picker, a set this large gives every arena its own shard, as
 * when each arena embedded one, instead of making arenas share shards once
 * their indices pass the slot count.  Slots are lazily initialized, so an
 * unused slot costs only its pointer.  This is to be changed in the future
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
	/* Number of slots available to the picker. */
	unsigned nshards;
	hpa_shard_set_pick_t pick;
	/*
	 * nshards slots indexed by owner id, allocated from the base at boot.
	 * Slots start NULL; a slot's shard is created when it is first picked.
	 */
	atomic_p_t *shards;
	hpa_shard_sets_t *sets;

	/* Options each shard in this set is initialized with. */
	hpa_shard_opts_t opts;
	sec_opts_t sec_opts;

	/* Serializes initialization of NULL shard slots. */
	malloc_mutex_t init_mtx;

	/*
	 * Used only by the roundrobin picker, which writes it on every pick.
	 * It is not cache-line isolated, so under roundrobin it may false-share
	 * with the next set's hot fields in hpa_shard_sets_t; to be measured.
	 */
	atomic_u_t rr_next;
};

/* Configuration used to construct a shard set. */
typedef struct hpa_shard_set_config_s hpa_shard_set_config_t;
struct hpa_shard_set_config_s {
	/* Inclusive allocation-size upper bound. */
	size_t size_max;
	unsigned nshards;
	/* Each set has its own picker; 0 is hpa_shard_set_pick_arena. */
	hpa_shard_set_pick_t pick;
};

typedef struct hpa_shard_sets_config_s hpa_shard_sets_config_t;
struct hpa_shard_sets_config_s {
	unsigned nsets;
	hpa_shard_set_config_t sets[HPA_MAX_SHARD_SETS];
};

struct hpa_shard_sets_s {
	bool initialized;
	unsigned nsets;

	/* Shared inputs retained for lazy shard initialization. */
	base_t *base;
	hpa_central_t *central;
	emap_t *emap;
	edata_cache_t *edata_cache;

	hpa_shard_set_t sets[HPA_MAX_SHARD_SETS];
};

/*
 * Resolves an owner id recorded on a pageslab.  Ids are unique only within a
 * shard set, so the caller supplies the set pointer.
 */
static inline hpa_shard_t *
hpa_shard_set_shard_from_id(hpa_shard_set_t *shard_set, unsigned id) {
	assert(shard_set != NULL);
	assert(id < shard_set->nshards);
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &shard_set->shards[id], ATOMIC_ACQUIRE);
	return shard;
}

bool hpa_shard_sets_config_validate(
    const hpa_shard_sets_config_t *config, size_t slab_max_alloc);

/*
 * Appends a set covering [size_start, size_end] with nshards shards chosen by
 * pick.  Returns true, leaving config unchanged, if config is full or the
 * range does not start right after the previous set's bound.
 */
bool hpa_shard_sets_config_add(hpa_shard_sets_config_t *config,
    size_t size_start, size_t size_end, unsigned nshards,
    hpa_shard_set_pick_t pick);

/*
 * Sets *pick to the picker named by the len bytes at name, which must match a
 * name in hpa_shard_set_pick_type_names exactly.  Returns true if none does.
 */
bool hpa_shard_set_pick_from_name(
    const char *name, size_t len, hpa_shard_set_pick_t *pick);

/*
 * Build the shard sets.  Returns true on error, without printing, leaving no
 * sets for the fork hooks to visit.  Slot arrays and lazily created shards are
 * allocated from base, and every shard shares central, emap and edata_cache;
 * since shards are never destroyed, all of these must outlive the sets.  Every
 * set's shards are created with opts and sec_opts.
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
 * Selects a slot in the set and initializes its shard if necessary.  affinity
 * is a caller-chosen key that keeps related requests on the same shard: the
 * arena picker uses affinity % nshards, and roundrobin ignores it.
 */
hpa_shard_t *hpa_shard_set_pick_shard(
    tsdn_t *tsdn, hpa_shard_set_t *shard_set, unsigned affinity);

/*
 * Fork hooks, called in the same numbered stages as the arena fork hooks so
 * lock ranks interleave correctly.  prefork2 takes every set's init_mtx before
 * locking any shard, which freezes the set of initialized shards for the rest
 * of the fork.  The base, central, emap and edata_cache passed to boot are
 * the caller's to fork.
 */
void hpa_shard_sets_prefork2(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_prefork3(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_prefork4(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_postfork_parent(tsdn_t *tsdn, hpa_shard_sets_t *sets);
void hpa_shard_sets_postfork_child(tsdn_t *tsdn, hpa_shard_sets_t *sets);

/*
 * The two composed: the whole of the allocation-side routing decision.  See
 * hpa_shard_set_pick_shard() for affinity.  Returns NULL if no set serves
 * usize or the picked shard cannot be initialized; the caller then falls back
 * to the PAC.
 */
JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_route(
    tsdn_t *tsdn, hpa_shard_sets_t *sets, size_t usize, unsigned affinity) {
	hpa_shard_set_t *shard_set = hpa_shard_sets_lookup(sets, usize);
	if (shard_set == NULL) {
		return NULL;
	}
	return hpa_shard_set_pick_shard(tsdn, shard_set, affinity);
}

#endif /* JEMALLOC_INTERNAL_HPA_SHARD_SET_H */
