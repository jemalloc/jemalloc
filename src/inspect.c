#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

void
inspect_extent_util_stats_get(tsdn_t *tsdn, const void *ptr, size_t *nfree,
    size_t *nregs, size_t *size) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL);

	const extent_t *extent = iealloc(tsdn, ptr);
	if (unlikely(extent == NULL)) {
		*nfree = *nregs = *size = 0;
		return;
	}

	*size = extent_size_get(extent);
	if (!extent_slab_get(extent)) {
		*nfree = 0;
		*nregs = 1;
	} else {
		*nfree = extent_nfree_get(extent);
		*nregs = bin_infos[extent_szind_get(extent)].nregs;
		assert(*nfree <= *nregs);
		assert(*nfree * extent_usize_get(extent) <= *size);
	}
}

void
inspect_extent_util_stats_verbose_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size, size_t *bin_nfree,
    size_t *bin_nregs, void **slabcur_addr) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL
	    && bin_nfree != NULL && bin_nregs != NULL && slabcur_addr != NULL);

	const extent_t *extent = iealloc(tsdn, ptr);
	if (unlikely(extent == NULL)) {
		*nfree = *nregs = *size = *bin_nfree = *bin_nregs = 0;
		*slabcur_addr = NULL;
		return;
	}

	*size = extent_size_get(extent);
	if (!extent_slab_get(extent)) {
		*nfree = *bin_nfree = *bin_nregs = 0;
		*nregs = 1;
		*slabcur_addr = NULL;
		return;
	}

	*nfree = extent_nfree_get(extent);
	const szind_t szind = extent_szind_get(extent);
	*nregs = bin_infos[szind].nregs;
	assert(*nfree <= *nregs);
	assert(*nfree * extent_usize_get(extent) <= *size);

	const arena_t *arena = (arena_t *)atomic_load_p(
	    &arenas[extent_arena_ind_get(extent)], ATOMIC_RELAXED);
	assert(arena != NULL);
	const unsigned binshard = extent_binshard_get(extent);
	bin_t *bin = &arena->bins[szind].bin_shards[binshard];

	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats) {
		*bin_nregs = *nregs * bin->stats.curslabs;
		assert(*bin_nregs >= bin->stats.curregs);
		*bin_nfree = *bin_nregs - bin->stats.curregs;
	} else {
		*bin_nfree = *bin_nregs = 0;
	}
	extent_t *slab;
	if (bin->slabcur != NULL) {
		slab = bin->slabcur;
	} else {
		slab = extent_heap_first(&bin->slabs_nonfull);
	}
	*slabcur_addr = slab != NULL ? extent_addr_get(slab) : NULL;
	malloc_mutex_unlock(tsdn, &bin->lock);
}
