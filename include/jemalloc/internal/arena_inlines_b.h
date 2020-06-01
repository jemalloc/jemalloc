#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_B_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_B_H

#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"

static inline arena_t *
arena_get_from_edata(edata_t *edata) {
	return (arena_t *)atomic_load_p(&arenas[edata_arena_ind_get(edata)],
	    ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE arena_t *
arena_choose_maybe_huge(tsd_t *tsd, arena_t *arena, size_t size) {
	if (arena != NULL) {
		return arena;
	}

	/*
	 * For huge allocations, use the dedicated huge arena if both are true:
	 * 1) is using auto arena selection (i.e. arena == NULL), and 2) the
	 * thread is not assigned to a manual arena.
	 */
	if (unlikely(size >= oversize_threshold)) {
		arena_t *tsd_arena = tsd_arena_get(tsd);
		if (tsd_arena == NULL || arena_is_auto(tsd_arena)) {
			return arena_choose_huge(tsd);
		}
	}

	return arena_choose(tsd, NULL);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_get(tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx,
    prof_info_t *prof_info, bool reset_recent) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(prof_info != NULL);

	edata_t *edata = NULL;
	bool is_slab;

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
		    ptr);
		is_slab = edata_slab_get(edata);
	} else if (unlikely(!(is_slab = alloc_ctx->slab))) {
		edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
		    ptr);
	}

	if (unlikely(!is_slab)) {
		/* edata must have been initialized at this point. */
		assert(edata != NULL);
		large_prof_info_get(tsd, edata, prof_info, reset_recent);
	} else {
		prof_info->alloc_tctx = (prof_tctx_t *)(uintptr_t)1U;
		/*
		 * No need to set other fields in prof_info; they will never be
		 * accessed if (uintptr_t)alloc_tctx == (uintptr_t)1U.
		 */
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset(tsd_t *tsd, const void *ptr,
    emap_alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd),
		    &arena_emap_global, ptr);
		if (unlikely(!edata_slab_get(edata))) {
			large_prof_tctx_reset(edata);
		}
	} else {
		if (unlikely(!alloc_ctx->slab)) {
			edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd),
			    &arena_emap_global, ptr);
			large_prof_tctx_reset(edata);
		}
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset_sampled(tsd_t *tsd, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global,
	    ptr);
	assert(!edata_slab_get(edata));

	large_prof_tctx_reset(edata);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_set(tsd_t *tsd, edata_t *edata, prof_tctx_t *tctx) {
	cassert(config_prof);

	assert(!edata_slab_get(edata));
	large_prof_info_set(edata, tctx);
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_ticks(tsdn_t *tsdn, arena_t *arena, unsigned nticks) {
	tsd_t *tsd;
	ticker_t *decay_ticker;

	if (unlikely(tsdn_null(tsdn))) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	decay_ticker = decay_ticker_get(tsd, arena_ind_get(arena));
	if (unlikely(decay_ticker == NULL)) {
		return;
	}
	if (unlikely(ticker_ticks(decay_ticker, nticks))) {
		arena_decay(tsdn, arena, false, false);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_tick(tsdn_t *tsdn, arena_t *arena) {
	arena_decay_ticks(tsdn, arena, 1);
}

JEMALLOC_ALWAYS_INLINE void *
arena_malloc(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind, bool zero,
    tcache_t *tcache, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);

	if (likely(tcache != NULL)) {
		if (likely(size <= SC_SMALL_MAXCLASS)) {
			return tcache_alloc_small(tsdn_tsd(tsdn), arena,
			    tcache, size, ind, zero, slow_path);
		}
		if (likely(size <= tcache_maxclass)) {
			return tcache_alloc_large(tsdn_tsd(tsdn), arena,
			    tcache, size, ind, zero, slow_path);
		}
		/* (size > tcache_maxclass) case falls through. */
		assert(size > tcache_maxclass);
	}

	return arena_malloc_hard(tsdn, arena, size, ind, zero);
}

JEMALLOC_ALWAYS_INLINE arena_t *
arena_aalloc(tsdn_t *tsdn, const void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	unsigned arena_ind = edata_arena_ind_get(edata);
	return (arena_t *)atomic_load_p(&arenas[arena_ind], ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_salloc(tsdn_t *tsdn, const void *ptr) {
	assert(ptr != NULL);
	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);

	return sz_index2size(alloc_ctx.szind);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_vsalloc(tsdn_t *tsdn, const void *ptr) {
	/*
	 * Return 0 if ptr is not within an extent managed by jemalloc.  This
	 * function has two extra costs relative to isalloc():
	 * - The rtree calls cannot claim to be dependent lookups, which induces
	 *   rtree lookup load dependencies.
	 * - The lookup may fail, so there is an extra branch to check for
	 *   failure.
	 */

	emap_full_alloc_ctx_t full_alloc_ctx;
	bool missing = emap_full_alloc_ctx_try_lookup(tsdn, &arena_emap_global,
	    ptr, &full_alloc_ctx);
	if (missing) {
		return 0;
	}

	if (full_alloc_ctx.edata == NULL) {
		return 0;
	}
	assert(edata_state_get(full_alloc_ctx.edata) == extent_state_active);
	/* Only slab members should be looked up via interior pointers. */
	assert(edata_addr_get(full_alloc_ctx.edata) == ptr
	    || edata_slab_get(full_alloc_ctx.edata));

	assert(full_alloc_ctx.szind != SC_NSIZES);

	return sz_index2size(full_alloc_ctx.szind);
}

static inline void
arena_dalloc_large_no_tcache(tsdn_t *tsdn, void *ptr, szind_t szind) {
	if (config_prof && unlikely(szind < SC_NBINS)) {
		arena_dalloc_promoted(tsdn, ptr, NULL, true);
	} else {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		large_dalloc(tsdn, edata);
	}
}

static inline void
arena_dalloc_no_tcache(tsdn_t *tsdn, void *ptr) {
	assert(ptr != NULL);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr, &alloc_ctx);

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, alloc_ctx.szind);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc_large(tsdn_t *tsdn, void *ptr, tcache_t *tcache, szind_t szind,
    bool slow_path) {
	if (szind < nhbins) {
		if (config_prof && unlikely(szind < SC_NBINS)) {
			arena_dalloc_promoted(tsdn, ptr, tcache, slow_path);
		} else {
			tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr, szind,
			    slow_path);
		}
	} else {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		large_dalloc(tsdn, edata);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc(tsdn_t *tsdn, void *ptr, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);

	if (unlikely(tcache == NULL)) {
		arena_dalloc_no_tcache(tsdn, ptr);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (caller_alloc_ctx != NULL) {
		alloc_ctx = *caller_alloc_ctx;
	} else {
		util_assume(!tsdn_null(tsdn));
		emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
		    &alloc_ctx);
	}

	if (config_debug) {
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.szind < SC_NSIZES);
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr,
		    alloc_ctx.szind, slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    slow_path);
	}
}

static inline void
arena_sdalloc_no_tcache(tsdn_t *tsdn, void *ptr, size_t size) {
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	emap_alloc_ctx_t alloc_ctx;
	if (!config_prof || !opt_prof) {
		/*
		 * There is no risk of being confused by a promoted sampled
		 * object, so base szind and slab on the given size.
		 */
		alloc_ctx.szind = sz_size2index(size);
		alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
	}

	if ((config_prof && opt_prof) || config_debug) {
		emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
		    &alloc_ctx);

		assert(alloc_ctx.szind == sz_size2index(size));
		assert((config_prof && opt_prof)
		    || alloc_ctx.slab == (alloc_ctx.szind < SC_NBINS));

		if (config_debug) {
			edata_t *edata = emap_edata_lookup(tsdn,
			    &arena_emap_global, ptr);
			assert(alloc_ctx.szind == edata_szind_get(edata));
			assert(alloc_ctx.slab == edata_slab_get(edata));
		}
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		arena_dalloc_small(tsdn, ptr);
	} else {
		arena_dalloc_large_no_tcache(tsdn, ptr, alloc_ctx.szind);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_sdalloc(tsdn_t *tsdn, void *ptr, size_t size, tcache_t *tcache,
    emap_alloc_ctx_t *caller_alloc_ctx, bool slow_path) {
	assert(!tsdn_null(tsdn) || tcache == NULL);
	assert(ptr != NULL);
	assert(size <= SC_LARGE_MAXCLASS);

	if (unlikely(tcache == NULL)) {
		arena_sdalloc_no_tcache(tsdn, ptr, size);
		return;
	}

	emap_alloc_ctx_t alloc_ctx;
	if (config_prof && opt_prof) {
		if (caller_alloc_ctx == NULL) {
			/* Uncommon case and should be a static check. */
			emap_alloc_ctx_lookup(tsdn, &arena_emap_global, ptr,
			    &alloc_ctx);
			assert(alloc_ctx.szind == sz_size2index(size));
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
		edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global,
		    ptr);
		assert(alloc_ctx.szind == edata_szind_get(edata));
		assert(alloc_ctx.slab == edata_slab_get(edata));
	}

	if (likely(alloc_ctx.slab)) {
		/* Small allocation. */
		tcache_dalloc_small(tsdn_tsd(tsdn), tcache, ptr,
		    alloc_ctx.szind, slow_path);
	} else {
		arena_dalloc_large(tsdn, ptr, tcache, alloc_ctx.szind,
		    slow_path);
	}
}

static inline void
arena_cache_oblivious_randomize(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    size_t alignment) {
	assert(edata_base_get(edata) == edata_addr_get(edata));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE -
		    lg_floor(CACHELINE_CEILING(alignment));
		size_t r;
		if (!tsdn_null(tsdn)) {
			tsd_t *tsd = tsdn_tsd(tsdn);
			r = (size_t)prng_lg_range_u64(
			    tsd_prng_statep_get(tsd), lg_range);
		} else {
			uint64_t stack_value = (uint64_t)(uintptr_t)&r;
			r = (size_t)prng_lg_range_u64(&stack_value, lg_range);
		}
		uintptr_t random_offset = ((uintptr_t)r) << (LG_PAGE -
		    lg_range);
		edata->e_addr = (void *)((uintptr_t)edata->e_addr +
		    random_offset);
		assert(ALIGNMENT_ADDR2BASE(edata->e_addr, alignment) ==
		    edata->e_addr);
	}
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_B_H */
