#define	JEMALLOC_HUGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

void *
huge_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero)
{

	assert(usize == s2u(usize));

	return (huge_palloc(tsdn, arena, usize, chunksize, zero));
}

void *
huge_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	size_t ausize;
	extent_t *extent;
	bool is_zeroed;

	/* Allocate one or more contiguous chunks for this request. */

	assert(!tsdn_null(tsdn) || arena != NULL);

	ausize = sa2u(usize, alignment);
	if (unlikely(ausize == 0 || ausize > HUGE_MAXCLASS))
		return (NULL);
	assert(ausize >= chunksize);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	if (likely(!tsdn_null(tsdn)))
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	if (unlikely(arena == NULL) || (extent = arena_chunk_alloc_huge(tsdn,
	    arena, usize, alignment, &is_zeroed)) == NULL)
		return (NULL);

	if (usize < extent_size_get(extent))
		extent_size_set(extent, usize);

	if (chunk_register(tsdn, extent)) {
		arena_chunk_dalloc_huge(tsdn, arena, extent_addr_get(extent),
		    usize);
		arena_extent_dalloc(tsdn, arena, extent);
		return (NULL);
	}

	/* Insert extent into huge. */
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	ql_elm_new(extent, ql_link);
	ql_tail_insert(&arena->huge, extent, ql_link);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (!is_zeroed)
			memset(extent_addr_get(extent), 0, usize);
	} else if (config_fill && unlikely(opt_junk_alloc))
		memset(extent_addr_get(extent), JEMALLOC_ALLOC_JUNK, usize);

	arena_decay_tick(tsdn, arena);
	return (extent_addr_get(extent));
}

#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk_impl)
#endif
static void
huge_dalloc_junk(tsdn_t *tsdn, void *ptr, size_t usize)
{

	if (config_fill && have_dss && unlikely(opt_junk_free)) {
		/*
		 * Only bother junk filling if the chunk isn't about to be
		 * unmapped.
		 */
		if (!config_munmap || (have_dss && chunk_in_dss(tsdn, ptr)))
			memset(ptr, JEMALLOC_FREE_JUNK, usize);
	}
}
#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk)
huge_dalloc_junk_t *huge_dalloc_junk = JEMALLOC_N(huge_dalloc_junk_impl);
#endif

static void
huge_ralloc_no_move_similar(tsdn_t *tsdn, extent_t *extent, size_t usize_min,
    size_t usize_max, bool zero)
{
	size_t usize, usize_next;
	arena_t *arena = extent_arena_get(extent);
	size_t oldsize = extent_size_get(extent);
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;
	bool pre_zeroed, post_zeroed;

	/* Increase usize to incorporate extra. */
	for (usize = usize_min; usize < usize_max && (usize_next = s2u(usize+1))
	    <= oldsize; usize = usize_next)
		; /* Do nothing. */

	if (oldsize == usize)
		return;

	pre_zeroed = extent_zeroed_get(extent);

	/* Fill if necessary (shrinking). */
	if (oldsize > usize) {
		size_t sdiff = oldsize - usize;
		if (config_fill && unlikely(opt_junk_free)) {
			memset((void *)((uintptr_t)extent_addr_get(extent) +
			    usize), JEMALLOC_FREE_JUNK, sdiff);
			post_zeroed = false;
		} else {
			post_zeroed = !chunk_purge_wrapper(tsdn, arena,
			    &chunk_hooks, extent_addr_get(extent),
			    CHUNK_CEILING(oldsize), usize, sdiff);
		}
	} else
		post_zeroed = pre_zeroed;

	/* Update the size of the huge allocation. */
	assert(extent_size_get(extent) != usize);
	chunk_deregister(tsdn, extent);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	extent_size_set(extent, usize);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
	chunk_reregister(tsdn, extent);
	/* Update zeroed. */
	extent_zeroed_set(extent, post_zeroed);

	arena_chunk_ralloc_huge_similar(tsdn, arena, extent, oldsize);

	/* Fill if necessary (growing). */
	if (oldsize < usize) {
		if (zero || (config_fill && unlikely(opt_zero))) {
			if (!pre_zeroed) {
				memset((void *)
				    ((uintptr_t)extent_addr_get(extent) +
				    oldsize), 0, usize - oldsize);
			}
		} else if (config_fill && unlikely(opt_junk_alloc)) {
			memset((void *)((uintptr_t)extent_addr_get(extent) +
			    oldsize), JEMALLOC_ALLOC_JUNK, usize - oldsize);
		}
	}
}

static bool
huge_ralloc_no_move_shrink(tsdn_t *tsdn, extent_t *extent, size_t usize)
{
	arena_t *arena = extent_arena_get(extent);
	size_t oldsize = extent_size_get(extent);
	chunk_hooks_t chunk_hooks = chunk_hooks_get(tsdn, arena);
	size_t cdiff = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);
	size_t sdiff = CHUNK_CEILING(usize) - usize;

	assert(oldsize > usize);

	/* Split excess chunks. */
	if (cdiff != 0) {
		extent_t *trail = chunk_split_wrapper(tsdn, arena, &chunk_hooks,
		    extent, CHUNK_CEILING(usize), cdiff);
		if (trail == NULL)
			return (true);

		if (config_fill && unlikely(opt_junk_free)) {
			huge_dalloc_junk(tsdn, extent_addr_get(trail),
			    extent_size_get(trail));
		}

		arena_chunk_cache_dalloc(tsdn, arena, &chunk_hooks,
		    extent_addr_get(trail), extent_size_get(trail),
		    extent_committed_get(trail));

		arena_extent_dalloc(tsdn, arena, trail);
	}

	/* Optionally fill trailing subchunk. */
	if (sdiff != 0) {
		bool post_zeroed;

		if (config_fill && unlikely(opt_junk_free)) {
			huge_dalloc_junk(tsdn,
			    (void *)((uintptr_t)extent_addr_get(extent) +
			    usize), sdiff);
			post_zeroed = false;
		} else {
			post_zeroed = !chunk_purge_wrapper(tsdn, arena,
			    &chunk_hooks, extent_addr_get(extent),
			    CHUNK_CEILING(usize), usize, sdiff);

			if (config_fill && unlikely(opt_zero) && !post_zeroed) {
				memset((void *)
				    ((uintptr_t)extent_addr_get(extent) +
				    usize), 0, sdiff);
			}
		}
		extent_zeroed_set(extent, post_zeroed);
	}

	arena_chunk_ralloc_huge_shrink(tsdn, arena, extent, oldsize);

	return (false);
}

static bool
huge_ralloc_no_move_expand(tsdn_t *tsdn, extent_t *extent, size_t usize,
    bool zero)
{
	arena_t *arena = extent_arena_get(extent);
	size_t oldsize = extent_size_get(extent);
	bool is_zeroed_subchunk = extent_zeroed_get(extent);
	bool is_zeroed_chunk = false;
	chunk_hooks_t chunk_hooks = chunk_hooks_get(tsdn, arena);
	size_t cdiff = CHUNK_CEILING(usize) - CHUNK_CEILING(oldsize);
	void *nchunk =
	    (void *)CHUNK_CEILING((uintptr_t)extent_past_get(extent));
	extent_t *trail;

	if ((trail = arena_chunk_cache_alloc(tsdn, arena, &chunk_hooks, nchunk,
	    cdiff, chunksize, &is_zeroed_chunk)) == NULL) {
		bool commit = true;
		if ((trail = chunk_alloc_wrapper(tsdn, arena, &chunk_hooks,
		    nchunk, cdiff, chunksize, &is_zeroed_chunk, &commit)) ==
		    NULL)
			return (true);
	}

	if (chunk_merge_wrapper(tsdn, arena, &chunk_hooks, extent, trail)) {
		arena_extent_dalloc(tsdn, arena, trail);
		chunk_dalloc_wrapper(tsdn, arena, &chunk_hooks,
		    extent_addr_get(trail), extent_size_get(trail),
		    extent_zeroed_get(trail), extent_committed_get(trail));
		return (true);
	}

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (!is_zeroed_subchunk) {
			memset((void *)((uintptr_t)extent_addr_get(extent) +
			    oldsize), 0, CHUNK_CEILING(oldsize) - oldsize);
		}
		if (!is_zeroed_chunk) {
			memset((void *)((uintptr_t)extent_addr_get(extent) +
			    CHUNK_CEILING(oldsize)), 0, usize -
			    CHUNK_CEILING(oldsize));
		}
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset((void *)((uintptr_t)extent_addr_get(extent) + oldsize),
		    JEMALLOC_ALLOC_JUNK, usize - oldsize);
	}

	if (usize < extent_size_get(extent))
		extent_size_set(extent, usize);

	arena_chunk_ralloc_huge_expand(tsdn, arena, extent, oldsize);

	return (false);
}

bool
huge_ralloc_no_move(tsdn_t *tsdn, extent_t *extent, size_t usize_min,
    size_t usize_max, bool zero)
{

	assert(s2u(extent_size_get(extent)) == extent_size_get(extent));
	/* The following should have been caught by callers. */
	assert(usize_min > 0 && usize_max <= HUGE_MAXCLASS);
	/* Both allocation sizes must be huge to avoid a move. */
	assert(extent_size_get(extent) >= chunksize && usize_max >= chunksize);

	if (CHUNK_CEILING(usize_max) > CHUNK_CEILING(extent_size_get(extent))) {
		/* Attempt to expand the allocation in-place. */
		if (!huge_ralloc_no_move_expand(tsdn, extent, usize_max,
		    zero)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
		/* Try again, this time with usize_min. */
		if (usize_min < usize_max && CHUNK_CEILING(usize_min) >
		    CHUNK_CEILING(extent_size_get(extent)) &&
		    huge_ralloc_no_move_expand(tsdn, extent, usize_min, zero)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
	}

	/*
	 * Avoid moving the allocation if the existing chunk size accommodates
	 * the new size.
	 */
	if (CHUNK_CEILING(extent_size_get(extent)) >= CHUNK_CEILING(usize_min)
	    && CHUNK_CEILING(extent_size_get(extent)) <=
	    CHUNK_CEILING(usize_max)) {
		huge_ralloc_no_move_similar(tsdn, extent, usize_min, usize_max,
		    zero);
		arena_decay_tick(tsdn, extent_arena_get(extent));
		return (false);
	}

	/* Attempt to shrink the allocation in-place. */
	if (CHUNK_CEILING(extent_size_get(extent)) > CHUNK_CEILING(usize_max)) {
		if (!huge_ralloc_no_move_shrink(tsdn, extent, usize_max)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
	}
	return (true);
}

static void *
huge_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero)
{

	if (alignment <= chunksize)
		return (huge_malloc(tsdn, arena, usize, zero));
	return (huge_palloc(tsdn, arena, usize, alignment, zero));
}

void *
huge_ralloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache)
{
	void *ret;
	size_t copysize;

	/* The following should have been caught by callers. */
	assert(usize > 0 && usize <= HUGE_MAXCLASS);
	/* Both allocation sizes must be huge to avoid a move. */
	assert(extent_size_get(extent) >= chunksize && usize >= chunksize);

	/* Try to avoid moving the allocation. */
	if (!huge_ralloc_no_move(tsdn, extent, usize, usize, zero))
		return (extent_addr_get(extent));

	/*
	 * usize and old size are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	ret = huge_ralloc_move_helper(tsdn, arena, usize, alignment, zero);
	if (ret == NULL)
		return (NULL);

	copysize = (usize < extent_size_get(extent)) ? usize :
	    extent_size_get(extent);
	memcpy(ret, extent_addr_get(extent), copysize);
	isdalloct(tsdn, extent, extent_addr_get(extent),
	    extent_size_get(extent), tcache, true);
	return (ret);
}

void
huge_dalloc(tsdn_t *tsdn, extent_t *extent)
{
	arena_t *arena;

	arena = extent_arena_get(extent);
	chunk_deregister(tsdn, extent);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	ql_remove(&arena->huge, extent, ql_link);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	huge_dalloc_junk(tsdn, extent_addr_get(extent),
	    extent_size_get(extent));
	arena_chunk_dalloc_huge(tsdn, extent_arena_get(extent),
	    extent_addr_get(extent), extent_size_get(extent));
	arena_extent_dalloc(tsdn, arena, extent);

	arena_decay_tick(tsdn, arena);
}

size_t
huge_salloc(tsdn_t *tsdn, const extent_t *extent)
{
	size_t size;
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	size = extent_size_get(extent);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	return (size);
}

prof_tctx_t *
huge_prof_tctx_get(tsdn_t *tsdn, const extent_t *extent)
{
	prof_tctx_t *tctx;
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	tctx = extent_prof_tctx_get(extent);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);

	return (tctx);
}

void
huge_prof_tctx_set(tsdn_t *tsdn, extent_t *extent, prof_tctx_t *tctx)
{
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->huge_mtx);
	extent_prof_tctx_set(extent, tctx);
	malloc_mutex_unlock(tsdn, &arena->huge_mtx);
}

void
huge_prof_tctx_reset(tsdn_t *tsdn, extent_t *extent)
{

	huge_prof_tctx_set(tsdn, extent, (prof_tctx_t *)(uintptr_t)1U);
}
