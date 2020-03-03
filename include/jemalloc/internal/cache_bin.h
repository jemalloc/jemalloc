#ifndef JEMALLOC_INTERNAL_CACHE_BIN_H
#define JEMALLOC_INTERNAL_CACHE_BIN_H

#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sz.h"

/*
 * The cache_bins are the mechanism that the tcache and the arena use to
 * communicate.  The tcache fills from and flushes to the arena by passing a
 * cache_bin_t to fill/flush.  When the arena needs to pull stats from the
 * tcaches associated with it, it does so by iterating over its
 * cache_bin_array_descriptor_t objects and reading out per-bin stats it
 * contains.  This makes it so that the arena need not know about the existence
 * of the tcache at all.
 */

/*
 * The size in bytes of each cache bin stack.  We also use this to indicate
 * *counts* of individual objects.
 */
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
	cache_bin_sz_t ncached_max;
};

typedef struct cache_bin_s cache_bin_t;
struct cache_bin_s {
	/* The value at the top of the stack. */
	void *stack_peek;
	/*
	 * The position of the top of the stack if the stack is non-empty, or
	 * one below it if not.
	 */
	void **stack_head;
	/*
	 * Keep the bin stats close to the data so that they have a higher
	 * chance of being on the same cacheline.
	 *
	 * This is logically public -- it is initialized to 0 during
	 * cache_bin_init, but is not otherwise touched.
	 */
	cache_bin_stats_t tstats;

	uint16_t low_bits_low_water;
	uint16_t low_bits_empty;
	uint16_t low_bits_full;
	uint16_t padding;
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
cache_bin_info_ncached_max(cache_bin_info_t *info) {
	return info->ncached_max;
}

static inline cache_bin_sz_t
cache_bin_ncached_get(cache_bin_t *bin, cache_bin_info_t *info) {
	cache_bin_sz_t ret = ((uint16_t)(uintptr_t)bin->stack_head
	    - bin->low_bits_empty) / sizeof(void *);
	assert(ret <= info->ncached_max);
	return ret;
}

static inline bool
cache_bin_empty(cache_bin_t *bin) {
	return bin->low_bits_empty == (uint16_t)(uintptr_t)bin->stack_head;
}

static inline void
cache_bin_assert_empty(cache_bin_t *bin, cache_bin_info_t *info) {
	/* We assert in two different ways which should be equivalent. */
	assert(cache_bin_empty(bin));
	assert(cache_bin_ncached_get(bin, info) == 0);
}

/* Returns the numeric value of low water in [0, ncached]. */
static inline cache_bin_sz_t
cache_bin_low_water_get(cache_bin_t *bin, cache_bin_info_t *info) {
	cache_bin_sz_t ret = (bin->low_bits_low_water - bin->low_bits_empty)
	    / sizeof(void *);
	assert(ret <= info->ncached_max);
	return ret;
}

/*
 * Indicates that the current cache bin position should be the low water mark
 * going forward.
 */
static inline void
cache_bin_low_water_set(cache_bin_t *bin) {
	bin->low_bits_low_water = (uint16_t)(uintptr_t)bin->stack_head;
}

static inline void
cache_bin_array_descriptor_init(cache_bin_array_descriptor_t *descriptor,
    cache_bin_t *bins_small, cache_bin_t *bins_large) {
	ql_elm_new(descriptor, link);
	descriptor->bins_small = bins_small;
	descriptor->bins_large = bins_large;
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_impl(cache_bin_t *bin, bool *success,
    bool adjust_low_water) {
	assert(cache_bin_empty(bin) || bin->stack_peek == *bin->stack_head);
	void *ret = bin->stack_peek;
	void **stack_head = bin->stack_head;
	uint16_t low_bits = (uint16_t)(uintptr_t)stack_head;
	void **new_stack_head = bin->stack_head - 1;
	/*
	 * Note: This also serves as an empty check, since low_bits_low_water
	 * is bounded by empty.
	 */
	if (unlikely(low_bits == bin->low_bits_low_water)) {
		if (adjust_low_water) {
			if (low_bits == bin->low_bits_empty) {
				*success = false;
				return NULL;
			}
			bin->low_bits_low_water =
			    (uint16_t)(uintptr_t)new_stack_head;
		} else {
			*success = false;
			return NULL;
		}
	}
	/*
	 * We do the read, even though the stack might now be empty.  To ensure
	 * that this is safe, we overallocate by 1 during initialization.
	 */
	bin->stack_peek = *new_stack_head;
	bin->stack_head = new_stack_head;
	*success = true;
	return ret;
}

JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy_reduced(cache_bin_t *bin, bool *success) {
	/* We don't look at info if we're not adjusting low-water. */
	return cache_bin_alloc_easy_impl(bin, success, false);
}

/*
 * We keep this alternate version around to allow nonintrusive experimentation
 * with alternate cache bin strategies that require the cache_bin_info_t.
 */
JEMALLOC_ALWAYS_INLINE void *
cache_bin_alloc_easy(cache_bin_t *bin, cache_bin_info_t *info, bool *success) {
	return cache_bin_alloc_easy_impl(bin, success, true);
}

JEMALLOC_ALWAYS_INLINE bool
cache_bin_dalloc_easy(cache_bin_t *bin, void *ptr) {
	assert(cache_bin_empty(bin) || bin->stack_peek == *bin->stack_head);
	void **stack_head = bin->stack_head;
	void **new_stack_head = stack_head + 1;
	uint16_t low_bits = (uint16_t)(uintptr_t)stack_head;
	if (unlikely(low_bits == bin->low_bits_full)) {
		return false;
	}
	bin->stack_peek = ptr;
	*new_stack_head = ptr;
	bin->stack_head = new_stack_head;
	return true;
}

typedef struct cache_bin_ptr_array_s cache_bin_ptr_array_t;
struct cache_bin_ptr_array_s {
	cache_bin_sz_t n;
	void **ptr;
};

#define CACHE_BIN_PTR_ARRAY_DECLARE(name, nval)				\
    cache_bin_ptr_array_t name;						\
    name.n = (nval)

static inline void
cache_bin_init_ptr_array_for_fill(cache_bin_t *bin, cache_bin_info_t *info,
    cache_bin_ptr_array_t *arr, cache_bin_sz_t nfill) {
	assert(cache_bin_ncached_get(bin, info) == 0);
	arr->ptr = bin->stack_head + 1;
}

/*
 * While nfill in cache_bin_init_ptr_array_for_fill is the number we *intend* to
 * fill, nfilled here is the number we actually filled (which may be less, in
 * case of OOM.
 */
static inline void
cache_bin_finish_fill(cache_bin_t *bin, cache_bin_info_t *info,
    cache_bin_ptr_array_t *arr, cache_bin_sz_t nfilled) {
	assert(cache_bin_ncached_get(bin, info) == 0);
	assert(nfilled <= info->ncached_max);

	if (nfilled == 0) {
		return;
	}
	bin->stack_head += nfilled;
	bin->stack_peek = *bin->stack_head;
}

static inline void
cache_bin_init_ptr_array_for_flush(cache_bin_t *bin, cache_bin_info_t *info,
    cache_bin_ptr_array_t *arr, cache_bin_sz_t nflush) {
	assert(nflush <= cache_bin_ncached_get(bin, info));
	arr->ptr = bin->stack_head;
}

/*
 * These accessors are used by the flush pathways -- they reverse ordinary array
 * ordering.
 */
JEMALLOC_ALWAYS_INLINE void *
cache_bin_ptr_array_get(cache_bin_ptr_array_t *arr, cache_bin_sz_t n) {
	return *(arr->ptr - n);
}

JEMALLOC_ALWAYS_INLINE void
cache_bin_ptr_array_set(cache_bin_ptr_array_t *arr, cache_bin_sz_t n, void *p) {
	*(arr->ptr - n) = p;
}

static inline void
cache_bin_finish_flush(cache_bin_t *bin, cache_bin_info_t *info,
    cache_bin_ptr_array_t *arr, cache_bin_sz_t nflushed) {
	assert(nflushed <= cache_bin_ncached_get(bin, info));
	bin->stack_head -= nflushed;
	bin->stack_peek = *bin->stack_head;

	uint16_t low_bits = (uint16_t)(uintptr_t)bin->stack_head;
	if (low_bits < bin->low_bits_low_water) {
		bin->low_bits_low_water = low_bits;
	}
}

/*
 * Initialize a cache_bin_info to represent up to the given number of items in
 * the cache_bins it is associated with.
 */
void cache_bin_info_init(cache_bin_info_t *bin_info,
    cache_bin_sz_t ncached_max);
/*
 * Given an array of initialized cache_bin_info_ts, determine how big an
 * allocation is required to initialize a full set of cache_bin_ts.
 */
void cache_bin_info_compute_alloc(cache_bin_info_t *infos, szind_t ninfos,
    size_t *size, size_t *alignment);

/*
 * Actually initialize some cache bins.  Callers should allocate the backing
 * memory indicated by a call to cache_bin_compute_alloc.  They should then
 * preincrement, call init once for each bin and info, and then call
 * cache_bin_postincrement.  *alloc_cur will then point immediately past the end
 * of the allocation.
 */
void cache_bin_preincrement(cache_bin_info_t *infos, szind_t ninfos,
    void *alloc, size_t *cur_offset);
void cache_bin_postincrement(cache_bin_info_t *infos, szind_t ninfos,
    void *alloc, size_t *cur_offset);
void cache_bin_init(cache_bin_t *bin, cache_bin_info_t *info, void *alloc,
    size_t *cur_offset);

/*
 * If a cache bin was zero initialized (either because it lives in static or
 * thread-local storage, or was memset to 0), this function indicates whether or
 * not cache_bin_init was called on it.
 */
bool cache_bin_still_zero_initialized(cache_bin_t *bin);

#endif /* JEMALLOC_INTERNAL_CACHE_BIN_H */
