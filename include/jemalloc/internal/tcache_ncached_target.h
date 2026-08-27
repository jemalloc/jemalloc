#ifndef JEMALLOC_INTERNAL_TCACHE_NCACHED_TARGET_H
#define JEMALLOC_INTERNAL_TCACHE_NCACHED_TARGET_H

#include "jemalloc/internal/cache_bin.h"

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_target_min(cache_bin_sz_t ncached_max) {
	assert(ncached_max > 0);
	return (ncached_max < 2) ? ncached_max : 2;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_target_max(cache_bin_sz_t ncached_max) {
	cache_bin_sz_t min = tcache_ncached_target_min(ncached_max);
	cache_bin_sz_t max = (cache_bin_sz_t)(ncached_max >> 1);
	return max < min ? min : max;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_fill_after_refill(
    cache_bin_sz_t nfill, cache_bin_sz_t ncached_max) {
	cache_bin_sz_t nfill_max = tcache_ncached_target_max(ncached_max);
	assert(nfill > 0 && nfill <= nfill_max);
	nfill = (cache_bin_sz_t)(nfill << 1);
	return nfill > nfill_max ? nfill_max : nfill;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_fill_after_underuse(
    cache_bin_sz_t nfill, cache_bin_sz_t ncached_max) {
	assert(nfill > 0 && nfill <= tcache_ncached_target_max(ncached_max));
	nfill >>= 1;
	cache_bin_sz_t nfill_min = tcache_ncached_target_min(ncached_max);
	return nfill < nfill_min ? nfill_min : nfill;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_retain_after_refill(cache_bin_sz_t nretain,
    cache_bin_sz_t nfilled, cache_bin_sz_t ncached_max) {
	assert(nretain > 0 && nretain <= ncached_max);
	assert(nfilled > 0 && nfilled <= ncached_max);
	cache_bin_sz_t target = nfilled > (cache_bin_sz_t)(ncached_max >> 1)
	    ? ncached_max
	    : (cache_bin_sz_t)(nfilled << 1);
	return nretain < target ? target : nretain;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_retain_after_gc(cache_bin_sz_t ncached,
    cache_bin_sz_t low_water, cache_bin_sz_t ncached_max) {
	assert(ncached_max > 0);
	assert(low_water <= ncached);
	assert(ncached <= ncached_max);

	cache_bin_sz_t used_since_gc = (cache_bin_sz_t)(ncached - low_water);
	if (used_since_gc == 0) {
		return tcache_ncached_target_min(ncached_max);
	}
	cache_bin_sz_t headroom = (cache_bin_sz_t)(used_since_gc >> 2);
	if (headroom == 0) {
		headroom = 1;
	}
	return used_since_gc > (cache_bin_sz_t)(ncached_max - headroom)
	    ? ncached_max
	    : (cache_bin_sz_t)(used_since_gc + headroom);
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_retain_after_overflow(
    cache_bin_sz_t nretain, cache_bin_sz_t ncached_max) {
	assert(nretain > 0 && nretain <= ncached_max);
	nretain >>= 1;
	cache_bin_sz_t nretain_min = tcache_ncached_target_min(ncached_max);
	if (nretain < nretain_min) {
		nretain = nretain_min;
	}
	assert(nretain > 0 && nretain <= ncached_max);
	return nretain;
}

JEMALLOC_ALWAYS_INLINE cache_bin_sz_t
tcache_ncached_flush_remain(
    cache_bin_sz_t nretain, cache_bin_sz_t ncached_max) {
	assert(nretain > 0 && nretain <= ncached_max);
	/* Leave room for the allocation whose dalloc detected the overflow. */
	return nretain < ncached_max
	    ? nretain
	    : (cache_bin_sz_t)(ncached_max - 1);
}

#endif /* JEMALLOC_INTERNAL_TCACHE_NCACHED_TARGET_H */
