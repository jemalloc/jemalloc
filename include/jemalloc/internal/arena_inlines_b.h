#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_B_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_B_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena_externs.h"
#include "jemalloc/internal/arena_structs.h"
#include "jemalloc/internal/bin_inlines.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/jemalloc_internal_inlines_b.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/large_externs.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof_externs.h"
#include "jemalloc/internal/prof_structs.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"

static inline arena_t *
arena_get_from_edata(const edata_t *edata) {
	return (arena_t *)atomic_load_p(
	    &arenas[edata_arena_ind_get(edata)], ATOMIC_RELAXED);
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
	arena_t *tsd_arena = tsd_arena_get(tsd);
	if (tsd_arena == NULL) {
		tsd_arena = arena_choose(tsd, NULL);
	}

	size_t threshold = atomic_load_zu(
	    &tsd_arena->pa_shard.pac.oversize_threshold, ATOMIC_RELAXED);
	if (unlikely(size >= threshold) && arena_is_auto(tsd_arena)) {
		return arena_choose_huge(tsd);
	}

	return tsd_arena;
}

JEMALLOC_ALWAYS_INLINE bool
large_dalloc_safety_checks(edata_t *edata, const void *ptr, size_t input_size) {
	if (!config_opt_safety_checks) {
		return false;
	}

	/*
	 * Eagerly detect double free and sized dealloc bugs for large sizes.
	 * The cost is low enough (as edata will be accessed anyway) to be
	 * enabled all the time.
	 */
	if (unlikely(edata == NULL
	        || edata_state_get(edata) != extent_state_active)) {
		safety_check_fail(
		    "Invalid deallocation detected: "
		    "pages being freed (%p) not currently active, "
		    "possibly caused by double free bugs.",
		    ptr);
		return true;
	}
	if (unlikely(input_size != edata_usize_get(edata)
	        || input_size > SC_LARGE_MAXCLASS)) {
		safety_check_fail_sized_dealloc(/* current_dealloc */ true, ptr,
		    /* true_size */ edata_usize_get(edata), input_size);
		return true;
	}

	return false;
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_get(tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx,
    prof_info_t *prof_info, bool reset_recent) {
	cassert(config_prof);
	assert(ptr != NULL);
	assert(prof_info != NULL);

	edata_t *edata = NULL;
	bool     is_slab;

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata = emap_edata_lookup(
		    tsd_tsdn(tsd), &arena_emap_global, ptr);
		is_slab = edata_slab_get(edata);
	} else if (unlikely(!(is_slab = alloc_ctx->slab))) {
		edata = emap_edata_lookup(
		    tsd_tsdn(tsd), &arena_emap_global, ptr);
	}

	if (unlikely(!is_slab)) {
		/* edata must have been initialized at this point. */
		assert(edata != NULL);
		size_t usize = (alloc_ctx == NULL)
		    ? edata_usize_get(edata)
		    : emap_alloc_ctx_usize_get(alloc_ctx);
		if (reset_recent
		    && large_dalloc_safety_checks(edata, ptr, usize)) {
			prof_info->alloc_tctx = PROF_TCTX_SENTINEL;
			return;
		}
		large_prof_info_get(tsd, edata, prof_info, reset_recent);
	} else {
		prof_info->alloc_tctx = PROF_TCTX_SENTINEL;
		/*
		 * No need to set other fields in prof_info; they will never be
		 * accessed if alloc_tctx == PROF_TCTX_SENTINEL.
		 */
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset(
    tsd_t *tsd, const void *ptr, emap_alloc_ctx_t *alloc_ctx) {
	cassert(config_prof);
	assert(ptr != NULL);

	/* Static check. */
	if (alloc_ctx == NULL) {
		edata_t *edata = emap_edata_lookup(
		    tsd_tsdn(tsd), &arena_emap_global, ptr);
		if (unlikely(!edata_slab_get(edata))) {
			large_prof_tctx_reset(edata);
		}
	} else {
		if (unlikely(!alloc_ctx->slab)) {
			edata_t *edata = emap_edata_lookup(
			    tsd_tsdn(tsd), &arena_emap_global, ptr);
			large_prof_tctx_reset(edata);
		}
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_tctx_reset_sampled(tsd_t *tsd, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	edata_t *edata = emap_edata_lookup(
	    tsd_tsdn(tsd), &arena_emap_global, ptr);
	assert(!edata_slab_get(edata));

	large_prof_tctx_reset(edata);
}

JEMALLOC_ALWAYS_INLINE void
arena_prof_info_set(
    tsd_t *tsd, edata_t *edata, prof_tctx_t *tctx, size_t size) {
	cassert(config_prof);

	assert(!edata_slab_get(edata));
	large_prof_info_set(edata, tctx, size);
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_ticks(tsdn_t *tsdn, arena_t *arena, unsigned nticks) {
	if (unlikely(tsdn_null(tsdn))) {
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	/*
	 * We use the ticker_geom_t to avoid having per-arena state in the tsd.
	 * Instead of having a countdown-until-decay timer running for every
	 * arena in every thread, we flip a coin once per tick, whose
	 * probability of coming up heads is 1/nticks; this is effectively the
	 * operation of the ticker_geom_t.  Each arena has the same chance of a
	 * coinflip coming up heads (1/ARENA_DECAY_NTICKS_PER_UPDATE), so we can
	 * use a single ticker for all of them.
	 */
	ticker_geom_t *decay_ticker = tsd_arena_decay_tickerp_get(tsd);
	uint64_t      *prng_state = tsd_prng_statep_get(tsd);
	if (unlikely(ticker_geom_ticks(decay_ticker, prng_state, nticks,
	        tsd_reentrancy_level_get(tsd) > 0))) {
		arena_decay(tsdn, arena, false, false);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_decay_tick(tsdn_t *tsdn, arena_t *arena) {
	arena_decay_ticks(tsdn, arena, 1);
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

	return emap_alloc_ctx_usize_get(&alloc_ctx);
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
	bool                  missing = emap_full_alloc_ctx_try_lookup(
            tsdn, &arena_emap_global, ptr, &full_alloc_ctx);
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

	return edata_usize_get(full_alloc_ctx.edata);
}

static inline void
arena_cache_oblivious_randomize(
    tsdn_t *tsdn, arena_t *arena, edata_t *edata, size_t alignment) {
	assert(edata_base_get(edata) == edata_addr_get(edata));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE
		    - lg_floor(CACHELINE_CEILING(alignment));
		size_t r;
		if (!tsdn_null(tsdn)) {
			tsd_t *tsd = tsdn_tsd(tsdn);
			r = (size_t)prng_lg_range_u64(
			    tsd_prng_statep_get(tsd), lg_range);
		} else {
			uint64_t stack_value = (uint64_t)(uintptr_t)&r;
			r = (size_t)prng_lg_range_u64(&stack_value, lg_range);
		}
		uintptr_t random_offset = ((uintptr_t)r)
		    << (LG_PAGE - lg_range);
		edata->e_addr = (void *)((byte_t *)edata->e_addr
		    + random_offset);
		assert(ALIGNMENT_ADDR2BASE(edata->e_addr, alignment)
		    == edata->e_addr);
	}
}

static inline bin_t *
arena_get_bin(arena_t *arena, szind_t binind, unsigned binshard) {
	bin_t *shard0 = (bin_t *)((byte_t *)arena + arena_bin_offsets[binind]);
	return shard0 + binshard;
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_B_H */
