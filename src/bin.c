#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/witness.h"

/* Default set of bins used to store heaps of free regions. */
sharded_bins_t default_bins[SC_NBINS];

bool
bin_update_shard_size(unsigned bin_shard_sizes[SC_NBINS], size_t start_size,
    size_t end_size, size_t nshards) {
	if (nshards > BIN_SHARDS_MAX || nshards == 0) {
		return true;
	}

	if (start_size > SC_SMALL_MAXCLASS) {
		return false;
	}
	if (end_size > SC_SMALL_MAXCLASS) {
		end_size = SC_SMALL_MAXCLASS;
	}

	/* Compute the index since this may happen before sz init. */
	szind_t ind1 = sz_size2index_compute(start_size);
	szind_t ind2 = sz_size2index_compute(end_size);
	for (unsigned i = ind1; i <= ind2; i++) {
		bin_shard_sizes[i] = (unsigned)nshards;
	}

	return false;
}

void
bin_shard_sizes_boot(unsigned bin_shard_sizes[SC_NBINS]) {
	/* Load the default number of shards. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		bin_shard_sizes[i] = N_BIN_SHARDS_DEFAULT;
	}
}

bool
default_bins_boot(tsd_t *tsd, unsigned narenas_auto) {
	unsigned n_total_shards = 0;
	for (unsigned i = 0; i < SC_NBINS; i++) {
		n_total_shards += bin_infos[i].n_shards;
	}
	n_total_shards *= narenas_auto;

	/* Init the pointer array.  Actual bins are allocated on demand. */
	bin_t **bin_ptrs = base_alloc(tsd_tsdn(tsd), b0get(),
	    n_total_shards * sizeof(bin_t *), CACHELINE);
	if (bin_ptrs == NULL) {
		return true;
	}
	memset(bin_ptrs, 0, n_total_shards * sizeof(bin_t *));

	bin_t **next_bin = bin_ptrs;
	for (unsigned i = 0; i < SC_NBINS; i++) {
		default_bins[i].bin_shards = next_bin;

		unsigned nbins = bin_infos[i].n_shards * narenas_auto;
		default_bins[i].n_shards = nbins;
		next_bin += nbins;
	}
	assert(next_bin == bin_ptrs + n_total_shards);

	return false;
}

bool
bin_init(bin_t *bin) {
	if (malloc_mutex_init(&bin->lock, "bin", WITNESS_RANK_BIN,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	bin->slabcur = NULL;
	edata_heap_new(&bin->slabs_nonfull);
	edata_list_init(&bin->slabs_full);
	if (config_stats) {
		memset(&bin->stats, 0, sizeof(bin_stats_t));
	}
	return false;
}

void
bin_prefork(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_prefork(tsdn, &bin->lock);
}

void
bin_postfork_parent(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_parent(tsdn, &bin->lock);
}

void
bin_postfork_child(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_child(tsdn, &bin->lock);
}
