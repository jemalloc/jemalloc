#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_internal_inlines_c.h"
#include "jemalloc/internal/large.h"
#include "jemalloc/internal/malloc_dispatch.h"
#include "jemalloc/internal/malloc_dispatch_inlines.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/tcache.h"
#include "jemalloc/internal/tcache_inlines.h"

/******************************************************************************/

void
malloc_dispatch_dalloc_promoted(
    tsdn_t *tsdn, void *ptr, tcache_t *tcache, bool slow_path) {
	cassert(config_prof);
	assert(opt_prof);

	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	size_t bumped_usize = arena_prof_demote(tsdn, edata, ptr);
	szind_t bumped_ind = sz_size2index(bumped_usize);
	if (bumped_usize >= SC_LARGE_MINCLASS && tcache != NULL
	    && tcache_can_cache_large(tcache, bumped_ind)) {
		tcache_dalloc_large(
		    tsdn_tsd(tsdn), tcache, ptr, bumped_ind, slow_path);
	} else {
		large_dalloc(tsdn, edata);
	}
}

void *
malloc_dispatch_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, bool slab, tcache_t *tcache) {
	if (slab) {
		assert(sz_can_use_slab(usize));
		/* Small; alignment doesn't require special slab placement. */

		/* usize should be a result of sz_sa2u() */
		assert((usize & (alignment - 1)) == 0);

		/*
		 * Small usize can't come from an alignment larger than a page.
		 */
		assert(alignment <= PAGE);

		return malloc_dispatch_malloc(tsdn, arena, usize,
		    sz_size2index(usize), zero, slab, tcache, true);
	} else {
		if (likely(alignment <= CACHELINE)) {
			return large_malloc(tsdn, arena, usize, zero);
		} else {
			return large_palloc(
			    tsdn, arena, usize, alignment, zero);
		}
	}
}

static void *
malloc_dispatch_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, bool slab, tcache_t *tcache) {
	if (alignment == 0) {
		return malloc_dispatch_malloc(tsdn, arena, usize,
		    sz_size2index(usize), zero, slab, tcache, true);
	}
	usize = sz_sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}
	return ipalloct_explicit_slab(
	    tsdn, usize, alignment, zero, slab, tcache, arena);
}

void *
malloc_dispatch_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t alignment, bool zero, bool slab, tcache_t *tcache) {
	size_t usize = alignment == 0 ? sz_s2u(size) : sz_sa2u(size, alignment);
	if (unlikely(usize == 0 || size > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(slab)) {
		assert(sz_can_use_slab(usize));
		/* Try to avoid moving the allocation. */
		UNUSED size_t newsize;
		if (!arena_ralloc_no_move(
		        tsdn, ptr, oldsize, usize, 0, zero, &newsize)) {
			return ptr;
		}
	}

	if (oldsize >= SC_LARGE_MINCLASS && usize >= SC_LARGE_MINCLASS) {
		return large_ralloc(tsdn, arena, ptr, usize, alignment, zero,
		    tcache);
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	void *ret = malloc_dispatch_ralloc_move_helper(
	    tsdn, arena, usize, alignment, zero, slab, tcache);
	if (ret == NULL) {
		return NULL;
	}

	/*
	 * Junk/zero-filling were already done by ipalloc() / dispatch alloc.
	 */
	size_t copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, ptr, oldsize, tcache, NULL, true);
	return ret;
}
