#define JEMALLOC_LARGE_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/

void *
large_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero) {
	assert(usize == sz_s2u(usize));

	return large_palloc(tsdn, arena, usize, CACHELINE, zero);
}

void *
large_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero) {
	size_t ausize;
	edata_t *edata;
	bool is_zeroed;
	UNUSED bool idump JEMALLOC_CC_SILENCE_INIT(false);

	assert(!tsdn_null(tsdn) || arena != NULL);

	ausize = sz_sa2u(usize, alignment);
	if (unlikely(ausize == 0 || ausize > SC_LARGE_MAXCLASS)) {
		return NULL;
	}

	if (config_fill && unlikely(opt_zero)) {
		zero = true;
	}
	/*
	 * Copy zero into is_zeroed and pass the copy when allocating the
	 * extent, so that it is possible to make correct junk/zero fill
	 * decisions below, even if is_zeroed ends up true when zero is false.
	 */
	is_zeroed = zero;
	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose_maybe_huge(tsdn_tsd(tsdn), arena, usize);
	}
	if (unlikely(arena == NULL) || (edata = arena_extent_alloc_large(tsdn,
	    arena, usize, alignment, &is_zeroed)) == NULL) {
		return NULL;
	}

	/* See comments in arena_bin_slabs_full_insert(). */
	if (!arena_is_auto(arena)) {
		/* Insert edata into large. */
		malloc_mutex_lock(tsdn, &arena->large_mtx);
		edata_list_append(&arena->large, edata);
		malloc_mutex_unlock(tsdn, &arena->large_mtx);
	}

	if (zero) {
		assert(is_zeroed);
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset(edata_addr_get(edata), JEMALLOC_ALLOC_JUNK,
		    edata_usize_get(edata));
	}

	arena_decay_tick(tsdn, arena);
	return edata_addr_get(edata);
}

static void
large_dalloc_junk_impl(void *ptr, size_t size) {
	memset(ptr, JEMALLOC_FREE_JUNK, size);
}
large_dalloc_junk_t *JET_MUTABLE large_dalloc_junk = large_dalloc_junk_impl;

static void
large_dalloc_maybe_junk_impl(void *ptr, size_t size) {
	if (config_fill && have_dss && unlikely(opt_junk_free)) {
		/*
		 * Only bother junk filling if the extent isn't about to be
		 * unmapped.
		 */
		if (opt_retain || (have_dss && extent_in_dss(ptr))) {
			large_dalloc_junk(ptr, size);
		}
	}
}
large_dalloc_maybe_junk_t *JET_MUTABLE large_dalloc_maybe_junk =
    large_dalloc_maybe_junk_impl;

static bool
large_ralloc_no_move_shrink(tsdn_t *tsdn, edata_t *edata, size_t usize) {
	arena_t *arena = arena_get_from_edata(edata);
	size_t oldusize = edata_usize_get(edata);
	ehooks_t *ehooks = arena_get_ehooks(arena);
	size_t diff = edata_size_get(edata) - (usize + sz_large_pad);

	assert(oldusize > usize);

	if (ehooks_split_will_fail(ehooks)) {
		return true;
	}

	/* Split excess pages. */
	if (diff != 0) {
		edata_t *trail = extent_split_wrapper(tsdn, &arena->edata_cache,
		    ehooks, edata, usize + sz_large_pad, sz_size2index(usize),
		    false, diff, SC_NSIZES, false);
		if (trail == NULL) {
			return true;
		}

		if (config_fill && unlikely(opt_junk_free)) {
			large_dalloc_maybe_junk(edata_addr_get(trail),
			    edata_size_get(trail));
		}

		arena_extents_dirty_dalloc(tsdn, arena, ehooks, trail);
	}

	arena_extent_ralloc_large_shrink(tsdn, arena, edata, oldusize);

	return false;
}

static bool
large_ralloc_no_move_expand(tsdn_t *tsdn, edata_t *edata, size_t usize,
    bool zero) {
	arena_t *arena = arena_get_from_edata(edata);
	size_t oldusize = edata_usize_get(edata);
	ehooks_t *ehooks = arena_get_ehooks(arena);
	size_t trailsize = usize - oldusize;

	if (ehooks_merge_will_fail(ehooks)) {
		return true;
	}

	if (config_fill && unlikely(opt_zero)) {
		zero = true;
	}
	/*
	 * Copy zero into is_zeroed_trail and pass the copy when allocating the
	 * extent, so that it is possible to make correct junk/zero fill
	 * decisions below, even if is_zeroed_trail ends up true when zero is
	 * false.
	 */
	bool is_zeroed_trail = zero;
	bool commit = true;
	edata_t *trail;
	bool new_mapping;
	if ((trail = ecache_alloc(tsdn, arena, ehooks, &arena->ecache_dirty,
	    edata_past_get(edata), trailsize, 0, CACHELINE, false, SC_NSIZES,
	    &is_zeroed_trail, &commit)) != NULL
	    || (trail = ecache_alloc(tsdn, arena, ehooks, &arena->ecache_muzzy,
	    edata_past_get(edata), trailsize, 0, CACHELINE, false, SC_NSIZES,
	    &is_zeroed_trail, &commit)) != NULL) {
		if (config_stats) {
			new_mapping = false;
		}
	} else {
		if ((trail = ecache_alloc_grow(tsdn, arena, ehooks,
		    &arena->ecache_retained, edata_past_get(edata), trailsize,
		    0, CACHELINE, false, SC_NSIZES, &is_zeroed_trail, &commit))
			== NULL) {
			return true;
		}
		if (config_stats) {
			new_mapping = true;
		}
	}

	if (extent_merge_wrapper(tsdn, ehooks, &arena->edata_cache, edata,
	    trail)) {
		extent_dalloc_wrapper(tsdn, arena, ehooks, trail);
		return true;
	}
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	szind_t szind = sz_size2index(usize);
	edata_szind_set(edata, szind);
	rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)edata_addr_get(edata), szind, false);

	if (config_stats && new_mapping) {
		arena_stats_mapped_add(tsdn, &arena->stats, trailsize);
	}

	if (zero) {
		if (config_cache_oblivious) {
			/*
			 * Zero the trailing bytes of the original allocation's
			 * last page, since they are in an indeterminate state.
			 * There will always be trailing bytes, because ptr's
			 * offset from the beginning of the extent is a multiple
			 * of CACHELINE in [0 .. PAGE).
			 */
			void *zbase = (void *)
			    ((uintptr_t)edata_addr_get(edata) + oldusize);
			void *zpast = PAGE_ADDR2BASE((void *)((uintptr_t)zbase +
			    PAGE));
			size_t nzero = (uintptr_t)zpast - (uintptr_t)zbase;
			assert(nzero > 0);
			memset(zbase, 0, nzero);
		}
		assert(is_zeroed_trail);
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset((void *)((uintptr_t)edata_addr_get(edata) + oldusize),
		    JEMALLOC_ALLOC_JUNK, usize - oldusize);
	}

	arena_extent_ralloc_large_expand(tsdn, arena, edata, oldusize);

	return false;
}

bool
large_ralloc_no_move(tsdn_t *tsdn, edata_t *edata, size_t usize_min,
    size_t usize_max, bool zero) {
	size_t oldusize = edata_usize_get(edata);

	/* The following should have been caught by callers. */
	assert(usize_min > 0 && usize_max <= SC_LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(oldusize >= SC_LARGE_MINCLASS
	    && usize_max >= SC_LARGE_MINCLASS);

	if (usize_max > oldusize) {
		/* Attempt to expand the allocation in-place. */
		if (!large_ralloc_no_move_expand(tsdn, edata, usize_max,
		    zero)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
		/* Try again, this time with usize_min. */
		if (usize_min < usize_max && usize_min > oldusize &&
		    large_ralloc_no_move_expand(tsdn, edata, usize_min, zero)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
	}

	/*
	 * Avoid moving the allocation if the existing extent size accommodates
	 * the new size.
	 */
	if (oldusize >= usize_min && oldusize <= usize_max) {
		arena_decay_tick(tsdn, arena_get_from_edata(edata));
		return false;
	}

	/* Attempt to shrink the allocation in-place. */
	if (oldusize > usize_max) {
		if (!large_ralloc_no_move_shrink(tsdn, edata, usize_max)) {
			arena_decay_tick(tsdn, arena_get_from_edata(edata));
			return false;
		}
	}
	return true;
}

static void *
large_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero) {
	if (alignment <= CACHELINE) {
		return large_malloc(tsdn, arena, usize, zero);
	}
	return large_palloc(tsdn, arena, usize, alignment, zero);
}

void *
large_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache,
    hook_ralloc_args_t *hook_args) {
	edata_t *edata = iealloc(tsdn, ptr);

	size_t oldusize = edata_usize_get(edata);
	/* The following should have been caught by callers. */
	assert(usize > 0 && usize <= SC_LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(oldusize >= SC_LARGE_MINCLASS
	    && usize >= SC_LARGE_MINCLASS);

	/* Try to avoid moving the allocation. */
	if (!large_ralloc_no_move(tsdn, edata, usize, usize, zero)) {
		hook_invoke_expand(hook_args->is_realloc
		    ? hook_expand_realloc : hook_expand_rallocx, ptr, oldusize,
		    usize, (uintptr_t)ptr, hook_args->args);
		return edata_addr_get(edata);
	}

	/*
	 * usize and old size are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	void *ret = large_ralloc_move_helper(tsdn, arena, usize, alignment,
	    zero);
	if (ret == NULL) {
		return NULL;
	}

	hook_invoke_alloc(hook_args->is_realloc
	    ? hook_alloc_realloc : hook_alloc_rallocx, ret, (uintptr_t)ret,
	    hook_args->args);
	hook_invoke_dalloc(hook_args->is_realloc
	    ? hook_dalloc_realloc : hook_dalloc_rallocx, ptr, hook_args->args);

	size_t copysize = (usize < oldusize) ? usize : oldusize;
	memcpy(ret, edata_addr_get(edata), copysize);
	isdalloct(tsdn, edata_addr_get(edata), oldusize, tcache, NULL, true);
	return ret;
}

/*
 * junked_locked indicates whether the extent's data have been junk-filled, and
 * whether the arena's large_mtx is currently held.
 */
static void
large_dalloc_prep_impl(tsdn_t *tsdn, arena_t *arena, edata_t *edata,
    bool junked_locked) {
	if (!junked_locked) {
		/* See comments in arena_bin_slabs_full_insert(). */
		if (!arena_is_auto(arena)) {
			malloc_mutex_lock(tsdn, &arena->large_mtx);
			edata_list_remove(&arena->large, edata);
			malloc_mutex_unlock(tsdn, &arena->large_mtx);
		}
		large_dalloc_maybe_junk(edata_addr_get(edata),
		    edata_usize_get(edata));
	} else {
		/* Only hold the large_mtx if necessary. */
		if (!arena_is_auto(arena)) {
			malloc_mutex_assert_owner(tsdn, &arena->large_mtx);
			edata_list_remove(&arena->large, edata);
		}
	}
	arena_extent_dalloc_large_prep(tsdn, arena, edata);
}

static void
large_dalloc_finish_impl(tsdn_t *tsdn, arena_t *arena, edata_t *edata) {
	ehooks_t *ehooks = arena_get_ehooks(arena);
	arena_extents_dirty_dalloc(tsdn, arena, ehooks, edata);
}

void
large_dalloc_prep_junked_locked(tsdn_t *tsdn, edata_t *edata) {
	large_dalloc_prep_impl(tsdn, arena_get_from_edata(edata), edata, true);
}

void
large_dalloc_finish(tsdn_t *tsdn, edata_t *edata) {
	large_dalloc_finish_impl(tsdn, arena_get_from_edata(edata), edata);
}

void
large_dalloc(tsdn_t *tsdn, edata_t *edata) {
	arena_t *arena = arena_get_from_edata(edata);
	large_dalloc_prep_impl(tsdn, arena, edata, false);
	large_dalloc_finish_impl(tsdn, arena, edata);
	arena_decay_tick(tsdn, arena);
}

size_t
large_salloc(tsdn_t *tsdn, const edata_t *edata) {
	return edata_usize_get(edata);
}

void
large_prof_info_get(const edata_t *edata, prof_info_t *prof_info) {
	edata_prof_info_get(edata, prof_info);
}

static void
large_prof_tctx_set(edata_t *edata, prof_tctx_t *tctx) {
	edata_prof_tctx_set(edata, tctx);
}

void
large_prof_tctx_reset(edata_t *edata) {
	large_prof_tctx_set(edata, (prof_tctx_t *)(uintptr_t)1U);
}

void
large_prof_info_set(edata_t *edata, prof_tctx_t *tctx) {
	large_prof_tctx_set(edata, tctx);
	nstime_t t;
	nstime_init_update(&t);
	edata_prof_alloc_time_set(edata, &t);
}
