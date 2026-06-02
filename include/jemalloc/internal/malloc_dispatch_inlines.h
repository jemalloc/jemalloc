#ifndef JEMALLOC_INTERNAL_MALLOC_DISPATCH_INLINES_H
#define JEMALLOC_INTERNAL_MALLOC_DISPATCH_INLINES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arena_inlines_b.h"
#include "jemalloc/internal/bin_inlines.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_internal_inlines_b.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/large.h"
#include "jemalloc/internal/malloc_dispatch.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/tcache_inlines.h"

JEMALLOC_ALWAYS_INLINE void *
malloc_dispatch_malloc(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero, bool slab, tcache_t *tcache, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);

	if (likely(tcache != NULL)) {
		if (likely(slab)) {
			assert(sz_can_use_slab(size));
			return tcache_alloc_small(tsdn_tsd(tsdn), arena, tcache,
			    size, ind, zero, slow_path);
		} else if (likely(tcache_can_cache_large(tcache, ind))) {
			return tcache_alloc_large(tsdn_tsd(tsdn), arena, tcache,
			    size, ind, zero, slow_path);
		}
		/* (size > tcache_max) case falls through. */
	}

	return arena_malloc_hard(tsdn, arena, size, ind, zero, slab);
}

static inline void
malloc_dispatch_dalloc_large_no_tcache(
    tsdn_t *tsdn, void *ptr, szind_t szind, size_t usize) {
	/*
	 * szind both classifies small vs large and validates the extent --
	 * inactive extents have szind == SC_NSIZES.
	 */
	if (config_prof && unlikely(szind < SC_NBINS)) {
		malloc_dispatch_dalloc_promoted(tsdn, ptr, NULL, true);
	} else {
		edata_t *edata = emap_edata_lookup(
		    tsdn, &arena_emap_global, ptr);
		if (large_dalloc_safety_checks(edata, ptr, usize)) {
			/* See the comment in isfree. */
			return;
		}
		large_dalloc(tsdn, edata);
	}
}

static inline void
malloc_dispatch_dalloc_no_tcache(tsdn_t *tsdn, void *ptr) {
	assert(ptr != NULL);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(
		    tsdn, &arena_emap_global, ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
		assert(emap_alloc_ctx_usize_get(&alloc_ctx)
		    == edata_usize_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		malloc_dispatch_dalloc_large_no_tcache(
		    tsdn, ptr, alloc_ctx.szind,
		    emap_alloc_ctx_usize_get(&alloc_ctx));
	}
}

JEMALLOC_ALWAYS_INLINE void
malloc_dispatch_dalloc_large(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    szind_t szind, size_t usize, bool slow_path) {
	assert(!tsdn_null(tsdn) && tcache != NULL);
	bool is_sample_promoted = config_prof && szind < SC_NBINS;
	if (unlikely(is_sample_promoted)) {
		malloc_dispatch_dalloc_promoted(tsdn, ptr, tcache, slow_path);
	} else {
		if (tcache_can_cache_large(tcache, szind)) {
			tcache_dalloc_large(
			    tsdn_tsd(tsdn), tcache, ptr, szind, slow_path);
		} else {
			edata_t *edata = emap_edata_lookup(
			    tsdn, &arena_emap_global, ptr);
			if (large_dalloc_safety_checks(edata, ptr, usize)) {
				/* See the comment in isfree. */
				return;
			}
			large_dalloc(tsdn, edata);
		}
	}
}

JEMALLOC_ALWAYS_INLINE bool
malloc_dispatch_dalloc_small_safety_check(tsdn_t *tsdn, void *ptr) {
	if (!config_debug) {
		return false;
	}
	edata_t   *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	szind_t    binind = edata_szind_get(edata);
	div_info_t div_info = arena_binind_div_info[binind];
	/*
	 * Calls the internal function bin_slab_regind_impl because the
	 * safety check does not require a lock.
	 */
	size_t regind = bin_slab_regind_impl(&div_info, binind, edata, ptr);
	slab_data_t      *slab_data = edata_slab_data_get(edata);
	const bin_info_t *bin_info = &bin_infos[binind];
	assert(edata_nfree_get(edata) < bin_info->nregs);
	if (unlikely(!bitmap_get(
	        slab_data->bitmap, &bin_info->bitmap_info, regind))) {
		safety_check_fail(
		    "Invalid deallocation detected: the pointer being freed (%p) not "
		    "currently active, possibly caused by double free bugs.\n",
		    ptr);
		return true;
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE void
malloc_dispatch_dalloc(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);

	if (unlikely(tcache == NULL)) {
		malloc_dispatch_dalloc_no_tcache(tsdn, ptr);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (caller_alloc_ctx != NULL) {
		alloc_ctx = *caller_alloc_ctx;
	} else {
		util_assume(tsdn != NULL);
		emap_alloc_ctx_lookup(
		    tsdn, &arena_emap_global, ptr, &alloc_ctx);
	}

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(
		    tsdn, &arena_emap_global, ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
		assert(emap_alloc_ctx_usize_get(&alloc_ctx)
		    == edata_usize_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		if (malloc_dispatch_dalloc_small_safety_check(tsdn, ptr)) {
			return;
		}
		tcache_dalloc_small(
		    tsdn_tsd(tsdn), tcache, ptr, alloc_ctx.szind, slow_path);
	} else {
		malloc_dispatch_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    emap_alloc_ctx_usize_get(&alloc_ctx), slow_path);
	}
}

static inline void
malloc_dispatch_sdalloc_no_tcache(tsdn_t *tsdn, void *ptr, size_t size) {
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	emap_alloc_ctx_t alloc_ctx;
	if (!config_prof || !opt_prof) {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		szind_t szind = sz_size2index(size);
		emap_alloc_ctx_init(
		    &alloc_ctx, szind, (szind < SC_NBINS), size);
	}

	if ((config_prof && opt_prof) || config_debug) {
		emap_alloc_ctx_lookup(
		    tsdn, &arena_emap_global, ptr, &alloc_ctx);

		assert(alloc_ctx.szind == sz_size2index(size));
		assert((config_prof && opt_prof)
		    || alloc_ctx.slab == (alloc_ctx.szind < SC_NBINS));

		if (config_debug) {
			edata_t *edata = emap_edata_lookup(
			    tsdn, &arena_emap_global, ptr);
			assert(alloc_ctx.szind == edata_szind_get(edata));
			assert(alloc_ctx.slab == edata_slab_get(edata));
		}
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		malloc_dispatch_dalloc_large_no_tcache(
		    tsdn, ptr, alloc_ctx.szind,
		    emap_alloc_ctx_usize_get(&alloc_ctx));
	}
}

JEMALLOC_ALWAYS_INLINE void
malloc_dispatch_sdalloc(tsdn_t *tsdn, void *ptr, size_t size, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	if (unlikely(tcache == NULL)) {
		malloc_dispatch_sdalloc_no_tcache(tsdn, ptr, size);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (config_prof && opt_prof) {
		if (caller_alloc_ctx == NULL) {
			/* Uncommon case and should be a static check. */
			emap_alloc_ctx_lookup(
			    tsdn, &arena_emap_global, ptr, &alloc_ctx);
			assert(alloc_ctx.szind == sz_size2index(size));
			assert(emap_alloc_ctx_usize_get(&alloc_ctx) == size);
		} else {
			alloc_ctx = *caller_alloc_ctx;
		}
	} else {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		alloc_ctx.szind = sz_size2index(size);
		alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
	}

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(
		    tsdn, &arena_emap_global, ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.slab == edata_slab_get(edata));
		emap_alloc_ctx_init(
		    &alloc_ctx, alloc_ctx.szind, alloc_ctx.slab, sz_s2u(size));
		assert(emap_alloc_ctx_usize_get(&alloc_ctx)
		    == edata_usize_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		if (malloc_dispatch_dalloc_small_safety_check(tsdn, ptr)) {
			return;
		}
		tcache_dalloc_small(
		    tsdn_tsd(tsdn), tcache, ptr, alloc_ctx.szind, slow_path);
	} else {
		malloc_dispatch_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    sz_s2u(size), slow_path);
	}
}

#endif /* JEMALLOC_INTERNAL_MALLOC_DISPATCH_INLINES_H */
