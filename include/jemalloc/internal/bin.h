#ifndef JEMALLOC_INTERNAL_BIN_H
#define JEMALLOC_INTERNAL_BIN_H

#include "jemalloc/internal/bin_stats.h"
#include "jemalloc/internal/bin_types.h"
#include "jemalloc/internal/extent.h"
#include "jemalloc/internal/extent_types.h"
#include "jemalloc/internal/extent_structs.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/sc.h"

/*
 * A bin contains a set of extents that are currently being used for slab
 * allocations.
 */
typedef struct bin_s bin_t;
struct bin_s {
	/* All operations on bin_t fields require lock ownership. */
	malloc_mutex_t		lock;

	/*
	 * Current slab being used to service allocations of this bin's size
	 * class.  slabcur is independent of slabs_{nonfull,full}; whenever
	 * slabcur is reassigned, the previous slab must be deallocated or
	 * inserted into slabs_{nonfull,full}.
	 */
	extent_t		*slabcur;

	/*
	 * Heap of non-full slabs.  This heap is used to assure that new
	 * allocations come from the non-full slab that is oldest/lowest in
	 * memory.
	 */
	extent_heap_t		slabs_nonfull;

	/* List used to track full slabs. */
	extent_list_t		slabs_full;

	/* Bin statistics. */
	bin_stats_t	stats;
};

/* A set of sharded bins of the same size class. */
typedef struct bins_s bins_t;
struct bins_s {
	/* Sharded bins.  Dynamically sized. */
	bin_t *bin_shards;
};

void bin_shard_sizes_boot(unsigned bin_shards[SC_NBINS]);
bool bin_update_shard_size(unsigned bin_shards[SC_NBINS], size_t start_size,
    size_t end_size, size_t nshards);

/* Initializes a bin to empty.  Returns true on error. */
bool bin_init(bin_t *bin);

/* Forking. */
void bin_prefork(tsdn_t *tsdn, bin_t *bin);
void bin_postfork_parent(tsdn_t *tsdn, bin_t *bin);
void bin_postfork_child(tsdn_t *tsdn, bin_t *bin);

/* Stats. */
static inline void
bin_stats_merge(tsdn_t *tsdn, bin_stats_t *dst_bin_stats, bin_t *bin) {
	malloc_mutex_lock(tsdn, &bin->lock);
	malloc_mutex_prof_accum(tsdn, &dst_bin_stats->mutex_data, &bin->lock);
	dst_bin_stats->nmalloc += bin->stats.nmalloc;
	dst_bin_stats->ndalloc += bin->stats.ndalloc;
	dst_bin_stats->nrequests += bin->stats.nrequests;
	dst_bin_stats->curregs += bin->stats.curregs;
	dst_bin_stats->nfills += bin->stats.nfills;
	dst_bin_stats->nflushes += bin->stats.nflushes;
	dst_bin_stats->nslabs += bin->stats.nslabs;
	dst_bin_stats->reslabs += bin->stats.reslabs;
	dst_bin_stats->curslabs += bin->stats.curslabs;
	dst_bin_stats->nonfull_slabs += bin->stats.nonfull_slabs;
	malloc_mutex_unlock(tsdn, &bin->lock);
}

#endif /* JEMALLOC_INTERNAL_BIN_H */
