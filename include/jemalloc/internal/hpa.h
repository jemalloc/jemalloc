#ifndef JEMALLOC_INTERNAL_HPA_H
#define JEMALLOC_INTERNAL_HPA_H

#include "jemalloc/internal/exp_grow.h"
#include "jemalloc/internal/hpa_central.h"
#include "jemalloc/internal/pai.h"
#include "jemalloc/internal/psset.h"

/* Completely derived; only used by CTL. */
typedef struct hpa_shard_stats_s hpa_shard_stats_t;
struct hpa_shard_stats_s {
	psset_stats_t psset_stats;
	/* The stat version of the nevictions counter. */
	uint64_t nevictions;
};

typedef struct hpa_shard_s hpa_shard_t;
struct hpa_shard_s {
	/*
	 * pai must be the first member; we cast from a pointer to it to a
	 * pointer to the hpa_shard_t.
	 */
	pai_t pai;
	malloc_mutex_t grow_mtx;
	malloc_mutex_t mtx;
	/* The base metadata allocator. */
	base_t *base;
	/*
	 * This edata cache is the one we use when allocating a small extent
	 * from a pageslab.  The pageslab itself comes from the centralized
	 * allocator, and so will use its edata_cache.
	 */
	edata_cache_small_t ecs;

	psset_t psset;

	/*
	 * The largest size we'll allocate out of the shard.  For those
	 * allocations refused, the caller (in practice, the PA module) will
	 * fall back to the more general (for now) PAC, which can always handle
	 * any allocation request.
	 */
	size_t alloc_max;

	/*
	 * Slabs currently purged away.  They are hugepage-sized and
	 * hugepage-aligned, but have had pages_nohuge and pages_purge_forced
	 * called on them.
	 *
	 * Guarded by grow_mtx.
	 */
	hpdata_list_t unused_slabs;

	/*
	 * How many grow operations have occurred.
	 *
	 * Guarded by grow_mtx.
	 */
	uint64_t age_counter;

	/*
	 * Either NULL (if empty), or some integer multiple of a
	 * hugepage-aligned number of hugepages.  We carve them off one at a
	 * time to satisfy new pageslab requests.
	 *
	 * Guarded by grow_mtx.
	 */
	void *eden;
	size_t eden_len;

	/* The arena ind we're associated with. */
	unsigned ind;
	emap_t *emap;

	/*
	 * The number of times we've purged a hugepage.  Each eviction purges a
	 * single hugepage.
	 *
	 * Guarded by the grow mutex.
	 */
	uint64_t nevictions;
};

/*
 * Whether or not the HPA can be used given the current configuration.  This is
 * is not necessarily a guarantee that it backs its allocations by hugepages,
 * just that it can function properly given the system it's running on.
 */
bool hpa_supported();
bool hpa_shard_init(hpa_shard_t *shard, emap_t *emap, base_t *base,
    edata_cache_t *edata_cache, unsigned ind, size_t alloc_max);

void hpa_shard_stats_accum(hpa_shard_stats_t *dst, hpa_shard_stats_t *src);
void hpa_shard_stats_merge(tsdn_t *tsdn, hpa_shard_t *shard,
    hpa_shard_stats_t *dst);

/*
 * Notify the shard that we won't use it for allocations much longer.  Due to
 * the possibility of races, we don't actually prevent allocations; just flush
 * and disable the embedded edata_cache_small.
 */
void hpa_shard_disable(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_destroy(tsdn_t *tsdn, hpa_shard_t *shard);

/*
 * We share the fork ordering with the PA and arena prefork handling; that's why
 * these are 3 and 4 rather than 0 and 1.
 */
void hpa_shard_prefork3(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_prefork4(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_postfork_parent(tsdn_t *tsdn, hpa_shard_t *shard);
void hpa_shard_postfork_child(tsdn_t *tsdn, hpa_shard_t *shard);

#endif /* JEMALLOC_INTERNAL_HPA_H */
