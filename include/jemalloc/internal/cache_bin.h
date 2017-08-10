#ifndef JEMALLOC_INTERNAL_CACHE_BIN_H
#define JEMALLOC_INTERNAL_CACHE_BIN_H

/*
 * The count of the number of cached allocations in a bin.  We make this signed
 * so that negative numbers can encode "invalid" states (e.g. a low water mark
 * for a bin that has never been filled).
 */
typedef int32_t cache_bin_sz_t;

typedef struct cache_bin_stats_s cache_bin_stats_t;
struct cache_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t nrequests;
};

/*
 * Read-only information associated with each element of tcache_t's tbins array
 * is stored separately, mainly to reduce memory usage.
 */
typedef struct cache_bin_info_s cache_bin_info_t;
struct cache_bin_info_s {
	/* Upper limit on ncached. */
	cache_bin_sz_t ncached_max;
};

typedef struct cache_bin_s cache_bin_t;
struct cache_bin_s {
	/* Min # cached since last GC. */
	cache_bin_sz_t low_water;
	/* # of cached objects. */
	cache_bin_sz_t ncached;
	/*
	 * ncached and stats are both modified frequently.  Let's keep them
	 * close so that they have a higher chance of being on the same
	 * cacheline, thus less write-backs.
	 */
	cache_bin_stats_t tstats;
	/*
	 * Stack of available objects.
	 *
	 * To make use of adjacent cacheline prefetch, the items in the avail
	 * stack goes to higher address for newer allocations.  avail points
	 * just above the available space, which means that
	 * avail[-ncached, ... -1] are available items and the lowest item will
	 * be allocated first.
	 */
	void **avail;
};

JEMALLOC_ALWAYS_INLINE void *
cache_alloc_easy(cache_bin_t *bin, bool *success) {
	void *ret;

	if (unlikely(bin->ncached == 0)) {
		bin->low_water = -1;
		*success = false;
		return NULL;
	}
	/*
	 * success (instead of ret) should be checked upon the return of this
	 * function.  We avoid checking (ret == NULL) because there is never a
	 * null stored on the avail stack (which is unknown to the compiler),
	 * and eagerly checking ret would cause pipeline stall (waiting for the
	 * cacheline).
	 */
	*success = true;
	ret = *(bin->avail - bin->ncached);
	bin->ncached--;

	if (unlikely(bin->ncached < bin->low_water)) {
		bin->low_water = bin->ncached;
	}

	return ret;

}

#endif /* JEMALLOC_INTERNAL_CACHE_BIN_H */
