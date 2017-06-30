#ifndef JEMALLOC_INTERNAL_SIZED_ALLOC_REGION_H
#define JEMALLOC_INTERNAL_SIZED_ALLOC_REGION_H

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/pages.h"

/*
 * This file defines sized alloc regions.  These regions have portions of memory
 * dedicated to a particular size class.  When allocations come out of these
 * regions, we can quickly determine their size based only on their address and
 * a little bit of global metadata, thus avoiding any rtree lookups or (if we're
 * lucky) any other cache-unfriendly lookups at all.
 *
 * We orchestrate it so that zero-initialization is a valid state for the
 * sized_alloc_region_t.  This lets us avoid branching on
 * opt_sized_alloc_region.
 */

/*
 * SIZED_ALLOC_REGION_LG_NUM_SCS:
 *     The log of the number of size classes that can come out of the
 *     sized-alloc region.
 * SIZED_ALLOC_REGION_LG_SC_SIZE:
 *     The log of the size of virtual address space that we allocate to each
 *     size class within the sized-alloc region.
 */
#if LG_SIZEOF_PTR == 2
#  define SIZED_ALLOC_REGION_LG_NUM_SCS 4
#  define SIZED_ALLOC_REGION_LG_SC_SIZE 25
#elif LG_SIZEOF_PTR == 3
#  define SIZED_ALLOC_REGION_LG_NUM_SCS 5
#  define SIZED_ALLOC_REGION_LG_SC_SIZE 32
#else
#  error Got a weird value of LG_SIZEOF_PTR
#endif

/*
 * SIZED_ALLOC_REGION_LG_SIZE:
 *     The log of the total amount of space we allocate for the region.
 *
 * SIZED_ALLOC_REGION_NUM_SCS:
 * SIZED_ALLOC_REGION_SC_SIZE:
 * SIZED_ALLOC_REGION_SIZE:
 *     These correspond to the LG_ equivalents, but are converted out of log
 *     space.
 */
#define SIZED_ALLOC_REGION_LG_SIZE \
    (SIZED_ALLOC_REGION_LG_NUM_SCS + SIZED_ALLOC_REGION_LG_SC_SIZE)
#define SIZED_ALLOC_REGION_NUM_SCS (ZU(1) << SIZED_ALLOC_REGION_LG_NUM_SCS)
#define SIZED_ALLOC_REGION_SC_SIZE (ZU(1) << SIZED_ALLOC_REGION_LG_SC_SIZE)
#define SIZED_ALLOC_REGION_SIZE (ZU(1) << SIZED_ALLOC_REGION_LG_SIZE)

typedef struct sized_alloc_region_s sized_alloc_region_t;
struct sized_alloc_region_s {
	JEMALLOC_ALIGNED(CACHELINE)
	uintptr_t region_start;
	/*
	 * This is always either 0 (if uninitialized, or if initialization
	 * failed), or SIZED_ALLOC_REGION_SIZE.  Doing it this way, instead of
	 * having "bool initialized;", allows us to fold the initialization and
	 * range checking branches into one.
	 */
	size_t region_size;
	bool committed;

	/* How much space is used in each size class. */
	JEMALLOC_ALIGNED(CACHELINE)
	atomic_zu_t size_class_space_used[SIZED_ALLOC_REGION_NUM_SCS];
};

extern bool opt_sized_alloc_region;
extern sized_alloc_region_t sized_alloc_region_global;

void sized_alloc_region_init(sized_alloc_region_t *region);
#ifdef JEMALLOC_JET
/*
 * We need this only during testing, when we might spin up multiple of these,
 * and should be considerate of virtual address space.
 */
void sized_alloc_region_destroy(sized_alloc_region_t *region);
#endif

/*
 * Returns true if we found the address.  If alloc_ctx is non-null, we fill it
 * in.  Safe to call on zero-initialized regions.
 */
static inline bool
sized_alloc_region_lookup(sized_alloc_region_t *region, void *addrp,
    alloc_ctx_t *alloc_ctx) {
	uintptr_t addr = (uintptr_t)addrp;

	/* Note: wraparound possible if addr < region. */
	uintptr_t addr_offset = addr - region->region_start;

	if (unlikely(addr_offset >= region->region_size)) {
		return false;
	}
	if (alloc_ctx != NULL) {
		alloc_ctx->szind = (szind_t)(
		    addr_offset >> SIZED_ALLOC_REGION_LG_SC_SIZE);
		alloc_ctx->slab = true;
	}
	return true;
}

/*
 * This is safe to call on zero-initialized region, or one for whom
 * initialization failed.
 */
static inline void *
sized_alloc_region_bump_alloc(sized_alloc_region_t *region, size_t size,
    szind_t szind, bool slab, bool *zero, bool *commit) {
	assert(PAGE_CEILING(size) == size);
	/* Only slab allocations allowed in the sized-alloc region. */
	assert(slab);

	if (region->region_size == 0 || szind >= SIZED_ALLOC_REGION_NUM_SCS) {
		return NULL;
	}

	size_t prev = atomic_fetch_add_zu(&region->size_class_space_used[szind],
	    size, ATOMIC_RELAXED);

	if (unlikely(prev + size >= SIZED_ALLOC_REGION_SC_SIZE)) {
		return NULL;
	}
	void *ret = (void *)(region->region_start
	    + (szind * SIZED_ALLOC_REGION_SC_SIZE) + prev);
	if (*commit) {
		if (pages_commit(ret, size)) {
			/* Just leak the virtual address space here. */
			return NULL;
		}
	} else {
		*commit = region->committed;
	}
	/* We get our memory straight from the OS, and never reuse it. */
	*zero = true;
	return ret;
}

#endif /* JEMALLOC_INTERNAL_SIZED_ALLOC_REGION_H */
