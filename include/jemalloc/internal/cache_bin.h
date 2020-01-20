#ifndef JEMALLOC_INTERNAL_CACHE_BIN_H
#define JEMALLOC_INTERNAL_CACHE_BIN_H

#include "jemalloc/internal/ql.h"

/*
 * The cache_bins are the mechanism that the tcache and the arena use to
 * communicate.  The tcache fills from and flushes to the arena by passing a
 * cache_bin_t to fill/flush.  When the arena needs to pull stats from the
 * tcaches associated with it, it does so by iterating over its
 * cache_bin_array_descriptor_t objects and reading out per-bin stats it
 * contains.  This makes it so that the arena need not know about the existence
 * of the tcache at all.
 */

/* The size in bytes of each cache bin stack. */
typedef uint16_t cache_bin_sz_t;

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
	/* The size of the bin stack, i.e. ncached_max * sizeof(ptr). */
	cache_bin_sz_t stack_size;
};
extern cache_bin_info_t	*tcache_bin_info;

typedef struct cache_bin_s cache_bin_t;
struct cache_bin_s {
	/*
	 * The cache bin stack is represented using 3 pointers: cur_ptr,
	 * low_water and full, optimized for the fast path efficiency.
	 *
	 * low addr ==> high addr
	 * |----|----|----|item1|item2|.....................|itemN|
	 *  full            cur                                    empty
	 * (ncached == N; full + ncached_max == empty)
	 *
	 * Data directly stored:
	 * 1) cur_ptr points to the current item to be allocated, i.e. *cur_ptr.
	 * 2) full points to the top of the stack (i.e. ncached == ncached_max),
	 * which is compared against on free_fastpath to check "is_full".
	 * 3) low_water indicates a low water mark of ncached.
	 * Range of low_water is [cur, empty], i.e. values of [ncached, 0].
	 *
	 * The empty position (ncached == 0) is derived via full + ncached_max
	 * and not accessed in the common case (guarded behind low_water).
	 *
	 * On 64-bit, 2 of the 3 pointers (full and low water) are compressed by
	 * omitting the high 32 bits.  Overflow of the half pointers is avoided
	 * when allocating / initializing the stack space.  As a result,
	 * cur_ptr.lowbits can be safely used for pointer comparisons.
	 */
	union {
		void **ptr;
		struct {
			/* highbits never accessed directly. */
#if (LG_SIZEOF_PTR == 3 && defined(JEMALLOC_BIG_ENDIAN))
			uint32_t __highbits;
#endif
			uint32_t lowbits;
#if (LG_SIZEOF_PTR == 3 && !defined(JEMALLOC_BIG_ENDIAN))
			uint32_t __highbits;
#endif
		};
	} cur_ptr;
	/*
	 * cur_ptr and stats are both modified frequently.  Let's keep them
	 * close so that they have a higher chance of being on the same
	 * cacheline, thus less write-backs.
	 */
	cache_bin_stats_t tstats;
	/*
	 * Points to the first item that hasn't been used since last GC, to
	 * track the low water mark (min # of cached).
	 */
	uint32_t low_water_position;
	/*
	 * Points to the position when the cache is full.
	 *
	 * To make use of adjacent cacheline prefetch, the items in the avail
	 * stack goes to higher address for newer allocations (i.e. cur_ptr++).
	 */
	uint32_t full_position;
};

typedef struct cache_bin_array_descriptor_s cache_bin_array_descriptor_t;
struct cache_bin_array_descriptor_s {
	/*
	 * The arena keeps a list of the cache bins associated with it, for
	 * stats collection.
	 */
	ql_elm(cache_bin_array_descriptor_t) link;
	/* Pointers to the tcache bins. */
	cache_bin_t *bins_small;
	cache_bin_t *bins_large;
};

/*
 * None of the cache_bin_*_get / _set functions is used on the fast path, which
 * relies on pointer comparisons to determine if the cache is full / empty.
 */

/* Returns ncached_max: Upper limit on ncached. */
static inline cache_bin_sz_t
cache_bin_ncached_max_get(szind_t ind) {
	return tcache_bin_info[ind].stack_size / sizeof(void *);
}

static inline cache_bin_sz_t
cache_bin_ncached_get(cache_bin_t *bin, szind_t ind) {
	cache_bin_sz_t n = (cache_bin_sz_t)((tcache_bin_info[ind].stack_size +
	    bin->full_position - bin->cur_ptr.lowbits) / sizeof(void *));
	assert(n <= cache_bin_ncached_max_get(ind));
	assert(n == 0 || *(bin->cur_ptr.ptr) != NULL);

	return n;
}

static inline void **
cache_bin_empty_position_get(cache_bin_t *bin, szind_t ind) {
	void **ret = bin->cur_ptr.ptr + cache_bin_ncached_get(bin, ind);
	/* Low bits overflow disallowed when allocating the space. */
	assert((uint32_t)(uintptr_t)ret >= bin->cur_ptr.lowbits);

	/* Can also be computed via (full_position + ncached_max) | highbits. */
	uintptr_t lowbits = bin->full_position +
	    tcache_bin_info[ind].stack_size;
	uintptr_t highbits = (uintptr_t)bin->cur_ptr.ptr &
	    ~(((uint64_t)1 << 32) - 1);
	assert(ret == (void **)(lowbits | highbits));

	return ret;
}

/* Returns the position of the bottom item on the stack; for convenience. */
static inline void **
cache_bin_bottom_item_get(cache_bin_t *bin, szind_t ind) {
	void **bottom = cache_bin_empty_position_get(bin, ind) - 1;
	assert(cache_bin_ncached_get(bin, ind) == 0 || *bottom != NULL);

	return bottom;
}

/* Returns the numeric value of low water in [0, ncached]. */
static inline cache_bin_sz_t
cache_bin_low_water_get(cache_bin_t *bin, szind_t ind) {
	cache_bin_sz_t ncached_max = cache_bin_ncached_max_get(ind);
	cache_bin_sz_t low_water = ncached_max -
	    (cache_bin_sz_t)((bin->low_water_position - bin->full_position) /
	    sizeof(void *));
	assert(low_water <= ncached_max);
	assert(low_water <= cache_bin_ncached_get(bin, ind));
	assert(bin->low_water_position >= bin->cur_ptr.lowbits);

	return low_water;
}

static inline void
cache_bin_ncached_set(cache_bin_t *bin, szind_t ind, cache_bin_sz_t n) {
	bin->cur_ptr.lowbits = bin->full_position +
	    tcache_bin_info[ind].stack_size - n * sizeof(void *);
	assert(n <= cache_bin_ncached_max_get(ind));
	assert(n == 0 || *bin->cur_ptr.ptr != NULL);
}

static inline void
cache_bin_array_descriptor_init(cache_bin_array_descriptor_t *descriptor,
    cache_bin_t *bins_small, cache_bin_t *bins_large) {
	ql_elm_new(descriptor, link);
	descriptor->bins_small = bins_small;
	descriptor->bins_large = bins_large;
}

#define INVALID_SZIND ((szind_t)(unsigned)-1)

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_impl(cache_bin_t *bin, bool *success, szind_t ind,
    const bool adjust_low_water) {
	/*
	 * This may read from the empty position; however the loaded value won't
	 * be used.  It's safe because the stack has one more slot reserved.
	 */
	void *ret = *(bin->cur_ptr.ptr++);
	/*
	 * Check for both bin->ncached == 0 and ncached < low_water in a single
	 * branch.  When adjust_low_water is true, this also avoids accessing
	 * tcache_bin_info (which is on a separate cacheline / page) in the
	 * common case.
	 */
	if (unlikely(bin->cur_ptr.lowbits > bin->low_water_position)) {
		if (adjust_low_water) {
			assert(ind != INVALID_SZIND);
			uint32_t empty_position = bin->full_position +
			    tcache_bin_info[ind].stack_size;
			if (unlikely(bin->cur_ptr.lowbits > empty_position)) {
				/* Over-allocated; revert. */
				bin->cur_ptr.ptr--;
				assert(bin->cur_ptr.lowbits == empty_position);
				*success = false;
				return NULL;
			}
			bin->low_water_position = bin->cur_ptr.lowbits;
		} else {
			assert(ind == INVALID_SZIND);
			bin->cur_ptr.ptr--;
			assert(bin->cur_ptr.lowbits == bin->low_water_position);
			*success = false;
			return NULL;
		}
	}

	/*
	 * success (instead of ret) should be checked upon the return of this
	 * function.  We avoid checking (ret == NULL) because there is never a
	 * null stored on the avail stack (which is unknown to the compiler),
	 * and eagerly checking ret would cause pipeline stall (waiting for the
	 * cacheline).
	 */
	*success = true;

	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_reduced(cache_bin_t *bin, bool *success) {
	/* The szind parameter won't be used. */
	return cache_bin_alloc_easy_impl(bin, success, INVALID_SZIND, false);
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy(cache_bin_t *bin, bool *success, szind_t ind) {
	return cache_bin_alloc_easy_impl(bin, success, ind, true);
}

#undef INVALID_SZIND

JEMALLOC_ALWAYS_INLINE bool
cache_bin_dalloc_easy(cache_bin_t *bin, void *ptr) {
	if (unlikely(bin->cur_ptr.lowbits == bin->full_position)) {
		return false;
	}

	*(--bin->cur_ptr.ptr) = ptr;
	assert(bin->cur_ptr.lowbits >= bin->full_position);

	return true;
}

#endif /* JEMALLOC_INTERNAL_CACHE_BIN_H */
