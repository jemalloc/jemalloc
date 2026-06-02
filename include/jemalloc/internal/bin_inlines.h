#ifndef JEMALLOC_INTERNAL_BIN_INLINES_H
#define JEMALLOC_INTERNAL_BIN_INLINES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/bin_info.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/mutex.h"

/* Stats. */
static inline void
bin_stats_nrequests_add(tsdn_t *tsdn, bin_t *bin, uint64_t n) {
	malloc_mutex_lock(tsdn, &bin->lock);
	bin->stats.nrequests += n;
	malloc_mutex_unlock(tsdn, &bin->lock);
}

static inline void
bin_stats_merge(tsdn_t *tsdn, bin_stats_data_t *dst_bin_stats, bin_t *bin) {
	malloc_mutex_lock(tsdn, &bin->lock);
	malloc_mutex_prof_accum(tsdn, &dst_bin_stats->mutex_data, &bin->lock);
	bin_stats_t *stats = &dst_bin_stats->stats_data;
	stats->nmalloc += bin->stats.nmalloc;
	stats->ndalloc += bin->stats.ndalloc;
	stats->nrequests += bin->stats.nrequests;
	stats->curregs += bin->stats.curregs;
	stats->nfills += bin->stats.nfills;
	stats->nflushes += bin->stats.nflushes;
	stats->nslabs += bin->stats.nslabs;
	stats->reslabs += bin->stats.reslabs;
	stats->curslabs += bin->stats.curslabs;
	stats->nonfull_slabs += bin->stats.nonfull_slabs;
	malloc_mutex_unlock(tsdn, &bin->lock);
}

/* Find the region index of a pointer within a slab. */
JEMALLOC_ALWAYS_INLINE size_t
bin_slab_regind_impl(const div_info_t *div_info, szind_t binind,
    const edata_t *slab, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)edata_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)edata_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab))
	        % (uintptr_t)bin_infos[binind].reg_size
	    == 0);

	diff = (size_t)((uintptr_t)ptr - (uintptr_t)edata_addr_get(slab));

	/* Avoid doing division with a variable divisor. */
	regind = div_compute(div_info, diff);
	assert(regind < bin_infos[binind].nregs);
	return regind;
}

JEMALLOC_ALWAYS_INLINE size_t
bin_slab_regind(const bin_dalloc_locked_info_t *info, szind_t binind,
    const edata_t *slab, const void *ptr) {
	size_t regind = bin_slab_regind_impl(
	    &info->div_info, binind, slab, ptr);
	return regind;
}

/*
 * Does the deallocation work associated with freeing a single pointer (a
 * "step") in between a bin_dalloc_locked begin and end call.
 *
 * Returns true if arena_slab_dalloc must be called on slab.  Doesn't do
 * stats updates, which happen during finish (this lets running counts get left
 * in a register).
 */
JEMALLOC_ALWAYS_INLINE bool
bin_dalloc_locked_step(tsdn_t *tsdn, bool is_auto, bin_t *bin,
    bin_dalloc_locked_info_t *info, szind_t binind, edata_t *slab,
    void *ptr) {
	const bin_info_t *bin_info = &bin_infos[binind];
	size_t            regind = bin_slab_regind(info, binind, slab, ptr);
	slab_data_t      *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	edata_nfree_inc(slab);

	if (config_stats) {
		info->ndalloc++;
	}

	unsigned nfree = edata_nfree_get(slab);
	if (nfree == bin_info->nregs) {
		bin_dalloc_locked_handle_newly_empty(
		    tsdn, is_auto, slab, bin);
		return true;
	} else if (nfree == 1 && slab != bin->slabcur) {
		bin_dalloc_locked_handle_newly_nonempty(
		    tsdn, is_auto, slab, bin);
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE void
bin_dalloc_locked_finish(tsdn_t *tsdn, bin_t *bin,
    bin_dalloc_locked_info_t *info) {
	if (config_stats) {
		bin->stats.ndalloc += info->ndalloc;
		assert(bin->stats.curregs >= (size_t)info->ndalloc);
		bin->stats.curregs -= (size_t)info->ndalloc;
	}
}

#endif /* JEMALLOC_INTERNAL_BIN_INLINES_H */
