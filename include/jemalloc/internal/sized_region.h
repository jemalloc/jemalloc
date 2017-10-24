#ifndef JEMALLOC_INTERNAL_SIZED_REGION_H
#define JEMALLOC_INTERNAL_SIZED_REGION_H

#include "jemalloc/internal/alloc_ctx.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/pages.h"

/*
 * This file defines sized regions.  These regions are portions of the address
 * space dedicated to a particular size class. When allocations come out of
 * these regions, we can quickly determine their size based only on their
 * address and a little bit of global metadata, thus avoiding any rtree lookups
 * or (if we're lucky) any other cache-unfriendly lookups at all.
 *
 * We orchestrate it so that zero-initialization is a valid state for the
 * sized_region_t.  This lets us avoid branching on opt_sized_regions.
 */

/* The number of size classes to put in the sized_region. */
#define SIZED_REGION_NUM_SCS NBINS

typedef struct sized_region_bin_s sized_region_bin_t;
struct JEMALLOC_ALIGNED(CACHELINE) sized_region_bin_s {
	/*
	 * We play a little fast and loose with memory orders for these two,
	 * since we can rely on the OS to maintain its own invariants, and since
	 * updates to dumpable are protected by dumpable_mtx.
	 */
	atomic_zu_t used;
	atomic_zu_t dumpable;
	malloc_mutex_t dumpable_mtx;
};

typedef struct sized_region_s sized_region_t;
struct sized_region_s {
	JEMALLOC_ALIGNED(CACHELINE)
	uintptr_t start;
	/*
	 * This is always either 0 (if uninitialized, or if initialization
	 * failed), or SIZED_REGION_SIZE.  Doing it this way, instead of having
	 * "bool initialized;", allows us to fold the initialization and range
	 * checking branches into one.
	 */
	size_t size;
	bool committed;

	/*
	 * Note that this is cacheline-aligned because of the struct definition,
	 * and so we need not align further to avoid sharing a cacheline with
	 * the metadata above.
	 */
	sized_region_bin_t bins[SIZED_REGION_NUM_SCS];
};

/* Whether this feature is enabled. */
extern bool opt_sized_regions;
/* The lg of the number of bytes reserved for each size class in the region. */
extern ssize_t opt_lg_sized_region_size;
/*
 * The region we actually allocate from.  There should be only one of these per
 * process (outside of unit tests).
 */
extern sized_region_t sized_region_global;

void sized_region_init(sized_region_t *region);
#ifdef JEMALLOC_JET
/*
 * We need this only during testing, when we might spin up multiple regions, and
 * should be considerate of virtual address space.
 */
void sized_region_destroy(sized_region_t *region);
#endif
bool sized_region_expand_dumpable(tsdn_t *tsdn, sized_region_t *region,
    size_t size, szind_t szind);

/*
 * Returns true if we found the address.  If alloc_ctx is non-null, we fill it
 * in.  Safe to call on zero-initialized regions.
 */
static inline bool
sized_region_lookup(sized_region_t *region, void *p, alloc_ctx_t *alloc_ctx) {
	uintptr_t addr = (uintptr_t)p;

	/* Note: wraparound possible if addr < region. */
	uintptr_t addr_offset = addr - region->start;

	if (unlikely(addr_offset >= region->size)) {
		LOG("sized_region.lookup.miss", "region is %p, ptr is %p",
		    region, p);
		return false;
	}
	if (alloc_ctx != NULL) {
		alloc_ctx->szind = (szind_t)(
		    addr_offset >> opt_lg_sized_region_size);
		alloc_ctx->slab = true;
	}
	LOG("sized_region.lookup.hit", "region is %p, ptr is %p, szind is %d, "
	    "slab is %d", region, p,
	    (int)(addr_offset >> opt_lg_sized_region_size), (int)true);

	return true;
}

/*
 * This is safe to call on zero-initialized region, or one for whom
 * initialization failed.
 */
static inline void *
sized_region_alloc(tsdn_t *tsdn, sized_region_t *region, size_t size,
    szind_t szind, bool slab, bool *zero, bool *commit) {
	assert(PAGE_CEILING(size) == size);
	/* Only slab allocations allowed in the sized-alloc region. */
	assert(slab);

	if (region->size == 0 || szind >= SIZED_REGION_NUM_SCS) {
		LOG("sized_region.alloc.failure.invalid",
		    "Can't alloc in given state: "
		    "region is %p, region size is %p, szind is %d", region,
		    (void *)region->size, szind);
		return NULL;
	}

	size_t sc_size = (ZU(1) << opt_lg_sized_region_size);

	/*
	 * Overflow should be at most a theoretical problem here, since
	 * presumably if the caller can't get memory from us, they'll get it
	 * from elsewhere, so we don't hit problems until we exhaust the address
	 * space.  For extra safety (and simplicity while debugging), we use a
	 * CAS loop instead of a fetch-add.
	 */
	size_t cur = atomic_load_zu(&region->bins[szind].used,
	    ATOMIC_RELAXED);
	size_t new_size;
	do {
		sized_region_bin_t *bin = &region->bins[szind];
		size_t dumpable = atomic_load_zu(&bin->dumpable,
		    ATOMIC_RELAXED);
		new_size = cur + size;
		if (new_size > sc_size) {
			LOG("sized_region.alloc.failure.exhausted",
			    "region is %p, szind is %d, sc_size is %p, "
			    "cur size is %p, size is %p", region, (int)szind,
			    (void *)sc_size, (void *)cur, (void *)size);
			return NULL;
		}
		if (new_size > dumpable) {
			if (sized_region_expand_dumpable(tsdn, region, size,
			    szind)) {
				return NULL;
			}
		}
	} while (!atomic_compare_exchange_weak_zu(
	    &region->bins[szind].used, &cur, new_size, ATOMIC_RELAXED,
	    ATOMIC_RELAXED));

	void *ret = (void *)(region->start + (szind * sc_size) + cur);
	if (*commit && !region->committed) {
		if (pages_commit(ret, size)) {
			LOG("sized_region.alloc.failure.commit",
			    "region is %p, ret is %p, size is %p",
			    region, ret, (void *)size);
			/* Just leak the virtual address space here. */
			return NULL;
		}
	} else {
		*commit = region->committed;
	}
	/* We get our memory straight from the OS, and never reuse it. */
	*zero = true;
	LOG("sized_region.alloc.success", "region is %p, ret is %p, "
	    "size is %p, szind is %d", region, ret, (void *)size, (int)szind);
	return ret;
}

#endif /* JEMALLOC_INTERNAL_SIZED_ALLOC_REGION_H */
