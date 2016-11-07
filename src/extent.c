#define	JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

rtree_t		extents_rtree;

static void	*extent_alloc_default(extent_hooks_t *extent_hooks,
    void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);
static bool	extent_dalloc_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static bool	extent_commit_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_decommit_default(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_split_default(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t size_a, size_t size_b, bool committed,
    unsigned arena_ind);
static bool	extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);

const extent_hooks_t	extent_hooks_default = {
	extent_alloc_default,
	extent_dalloc_default,
	extent_commit_default,
	extent_decommit_default,
	extent_purge_default,
	extent_split_default,
	extent_merge_default
};

/* Used exclusively for gdump triggering. */
static size_t	curpages;
static size_t	highpages;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	extent_record(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_heap_t extent_heaps[NPSIZES+1],
    bool cache, extent_t *extent);

/******************************************************************************/

extent_t *
extent_alloc(tsdn_t *tsdn, arena_t *arena)
{
	extent_t *extent;

	malloc_mutex_lock(tsdn, &arena->extent_cache_mtx);
	extent = ql_last(&arena->extent_cache, ql_link);
	if (extent == NULL) {
		malloc_mutex_unlock(tsdn, &arena->extent_cache_mtx);
		return (base_alloc(tsdn, sizeof(extent_t)));
	}
	ql_tail_remove(&arena->extent_cache, extent_t, ql_link);
	malloc_mutex_unlock(tsdn, &arena->extent_cache_mtx);
	return (extent);
}

void
extent_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent)
{

	malloc_mutex_lock(tsdn, &arena->extent_cache_mtx);
	ql_elm_new(extent, ql_link);
	ql_tail_insert(&arena->extent_cache, extent, ql_link);
	malloc_mutex_unlock(tsdn, &arena->extent_cache_mtx);
}

extent_hooks_t *
extent_hooks_get(arena_t *arena)
{

	return ((extent_hooks_t *)atomic_read_p(&arena->extent_hooks_pun));
}

extent_hooks_t *
extent_hooks_set(arena_t *arena, extent_hooks_t *extent_hooks)
{
	extent_hooks_t *old_extent_hooks = extent_hooks_get(arena);
	union {
		extent_hooks_t	**h;
		void		**v;
	} u;

	u.h = &arena->extent_hooks;
	atomic_write_p(u.v, extent_hooks);

	return (old_extent_hooks);
}

static void
extent_hooks_assure_initialized(arena_t *arena, extent_hooks_t **r_extent_hooks)
{

	if (*r_extent_hooks == EXTENT_HOOKS_INITIALIZER)
		*r_extent_hooks = extent_hooks_get(arena);
}

#ifdef JEMALLOC_JET
#undef extent_size_quantize_floor
#define	extent_size_quantize_floor JEMALLOC_N(n_extent_size_quantize_floor)
#endif
size_t
extent_size_quantize_floor(size_t size)
{
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert((size & PAGE_MASK) == 0);

	assert(size != 0);
	assert(size == PAGE_CEILING(size));

	pind = psz2ind(size - large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return (size);
	}
	ret = pind2sz(pind - 1) + large_pad;
	assert(ret <= size);
	return (ret);
}
#ifdef JEMALLOC_JET
#undef extent_size_quantize_floor
#define	extent_size_quantize_floor JEMALLOC_N(extent_size_quantize_floor)
extent_size_quantize_t *extent_size_quantize_floor =
    JEMALLOC_N(n_extent_size_quantize_floor);
#endif

#ifdef JEMALLOC_JET
#undef extent_size_quantize_ceil
#define	extent_size_quantize_ceil JEMALLOC_N(n_extent_size_quantize_ceil)
#endif
size_t
extent_size_quantize_ceil(size_t size)
{
	size_t ret;

	assert(size > 0);
	assert(size - large_pad <= LARGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = extent_size_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = pind2sz(psz2ind(ret - large_pad + 1)) + large_pad;
	}
	return (ret);
}
#ifdef JEMALLOC_JET
#undef extent_size_quantize_ceil
#define	extent_size_quantize_ceil JEMALLOC_N(extent_size_quantize_ceil)
extent_size_quantize_t *extent_size_quantize_ceil =
    JEMALLOC_N(n_extent_size_quantize_ceil);
#endif

JEMALLOC_INLINE_C int
extent_ad_comp(const extent_t *a, const extent_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

/* Generate pairing heap functions. */
ph_gen(, extent_heap_, extent_heap_t, extent_t, ph_link, extent_ad_comp)

static void
extent_heaps_insert(tsdn_t *tsdn, extent_heap_t extent_heaps[NPSIZES+1],
    extent_t *extent)
{
	size_t psz = extent_size_quantize_floor(extent_size_get(extent));
	pszind_t pind = psz2ind(psz);

	malloc_mutex_assert_owner(tsdn, &extent_arena_get(extent)->extents_mtx);

	extent_heap_insert(&extent_heaps[pind], extent);
}

static void
extent_heaps_remove(tsdn_t *tsdn, extent_heap_t extent_heaps[NPSIZES+1],
    extent_t *extent)
{
	size_t psz = extent_size_quantize_floor(extent_size_get(extent));
	pszind_t pind = psz2ind(psz);

	malloc_mutex_assert_owner(tsdn, &extent_arena_get(extent)->extents_mtx);

	extent_heap_remove(&extent_heaps[pind], extent);
}

static bool
extent_rtree_acquire(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const extent_t *extent, bool dependent, bool init_missing,
    rtree_elm_t **r_elm_a, rtree_elm_t **r_elm_b)
{

	*r_elm_a = rtree_elm_acquire(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_base_get(extent), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL)
		return (true);
	assert(*r_elm_a != NULL);

	if (extent_size_get(extent) > PAGE) {
		*r_elm_b = rtree_elm_acquire(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_last_get(extent), dependent,
		    init_missing);
		if (!dependent && *r_elm_b == NULL) {
			rtree_elm_release(tsdn, &extents_rtree, *r_elm_a);
			return (true);
		}
		assert(*r_elm_b != NULL);
	} else
		*r_elm_b = NULL;

	return (false);
}

static void
extent_rtree_write_acquired(tsdn_t *tsdn, rtree_elm_t *elm_a,
    rtree_elm_t *elm_b, const extent_t *extent)
{

	rtree_elm_write_acquired(tsdn, &extents_rtree, elm_a, extent);
	if (elm_b != NULL)
		rtree_elm_write_acquired(tsdn, &extents_rtree, elm_b, extent);
}

static void
extent_rtree_release(tsdn_t *tsdn, rtree_elm_t *elm_a, rtree_elm_t *elm_b)
{

	rtree_elm_release(tsdn, &extents_rtree, elm_a);
	if (elm_b != NULL)
		rtree_elm_release(tsdn, &extents_rtree, elm_b);
}

static void
extent_interior_register(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const extent_t *extent)
{
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE), extent);
	}
}

static bool
extent_register(tsdn_t *tsdn, const extent_t *extent)
{
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_elm_t *elm_a, *elm_b;

	if (extent_rtree_acquire(tsdn, rtree_ctx, extent, false, true, &elm_a,
	    &elm_b))
		return (true);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, extent);
	if (extent_slab_get(extent))
		extent_interior_register(tsdn, rtree_ctx, extent);
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof && opt_prof && extent_active_get(extent)) {
		size_t nadd = extent_size_get(extent) >> LG_PAGE;
		size_t cur = atomic_add_zu(&curpages, nadd);
		size_t high = atomic_read_zu(&highpages);
		while (cur > high && atomic_cas_zu(&highpages, high, cur)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highpages update race.
			 */
			high = atomic_read_zu(&highpages);
		}
		if (cur > high && prof_gdump_get_unlocked())
			prof_gdump(tsdn);
	}

	return (false);
}

static void
extent_reregister(tsdn_t *tsdn, const extent_t *extent)
{
	bool err = extent_register(tsdn, extent);
	assert(!err);
}

static void
extent_interior_deregister(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    const extent_t *extent)
{
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

static void
extent_deregister(tsdn_t *tsdn, extent_t *extent)
{
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_elm_t *elm_a, *elm_b;

	extent_rtree_acquire(tsdn, rtree_ctx, extent, true, false, &elm_a,
	    &elm_b);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, NULL);
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof && opt_prof && extent_active_get(extent)) {
		size_t nsub = extent_size_get(extent) >> LG_PAGE;
		assert(atomic_read_zu(&curpages) >= nsub);
		atomic_sub_zu(&curpages, nsub);
	}
}

/*
 * Do first-best-fit extent selection, i.e. select the lowest extent that best
 * fits.
 */
static extent_t *
extent_first_best_fit(tsdn_t *tsdn, arena_t *arena,
    extent_heap_t extent_heaps[NPSIZES+1], size_t size)
{
	pszind_t pind, i;

	malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);

	pind = psz2ind(extent_size_quantize_ceil(size));
	for (i = pind; i < NPSIZES+1; i++) {
		extent_t *extent = extent_heap_first(&extent_heaps[i]);
		if (extent != NULL)
			return (extent);
	}

	return (NULL);
}

static void
extent_leak(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    bool cache, extent_t *extent)
{

	/*
	 * Leak extent after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (cache) {
		extent_purge_wrapper(tsdn, arena, r_extent_hooks, extent, 0,
		    extent_size_get(extent));
	}
	extent_dalloc(tsdn, arena, extent);
}

static extent_t *
extent_recycle(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extent_heap_t extent_heaps[NPSIZES+1], bool locked, bool cache,
    void *new_addr, size_t usize, size_t pad, size_t alignment, bool *zero,
    bool *commit, bool slab)
{
	extent_t *extent;
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	size_t size, alloc_size, leadsize, trailsize;

	if (locked)
		malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);
	assert(new_addr == NULL || !slab);
	assert(pad == 0 || !slab);
	assert(alignment > 0);
	if (config_debug && new_addr != NULL) {
		extent_t *prev;

		/*
		 * Non-NULL new_addr has two use cases:
		 *
		 *   1) Recycle a known-extant extent, e.g. during purging.
		 *   2) Perform in-place expanding reallocation.
		 *
		 * Regardless of use case, new_addr must either refer to a
		 * non-existing extent, or to the base of an extant extent,
		 * since only active slabs support interior lookups (which of
		 * course cannot be recycled).
		 */
		assert(PAGE_ADDR2BASE(new_addr) == new_addr);
		assert(pad == 0);
		assert(alignment <= PAGE);
		prev = extent_lookup(tsdn, (void *)((uintptr_t)new_addr - PAGE),
		    false);
		assert(prev == NULL || extent_past_get(prev) == new_addr);
	}

	size = usize + pad;
	alloc_size = size + PAGE_CEILING(alignment) - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size < usize)
		return (NULL);
	if (!locked)
		malloc_mutex_lock(tsdn, &arena->extents_mtx);
	extent_hooks_assure_initialized(arena, r_extent_hooks);
	if (new_addr != NULL) {
		rtree_elm_t *elm;

		elm = rtree_elm_acquire(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)new_addr, false, false);
		if (elm != NULL) {
			extent = rtree_elm_read_acquired(tsdn, &extents_rtree,
			    elm);
			if (extent != NULL) {
				assert(extent_base_get(extent) == new_addr);
				if (extent_arena_get(extent) != arena ||
				    extent_size_get(extent) < size ||
				    extent_active_get(extent) ||
				    extent_retained_get(extent) == cache)
					extent = NULL;
			}
			rtree_elm_release(tsdn, &extents_rtree, elm);
		} else
			extent = NULL;
	} else {
		extent = extent_first_best_fit(tsdn, arena, extent_heaps,
		    alloc_size);
	}
	if (extent == NULL) {
		if (!locked)
			malloc_mutex_unlock(tsdn, &arena->extents_mtx);
		return (NULL);
	}
	extent_heaps_remove(tsdn, extent_heaps, extent);
	arena_extent_cache_maybe_remove(tsdn, arena, extent, cache);

	leadsize = ALIGNMENT_CEILING((uintptr_t)extent_base_get(extent),
	    PAGE_CEILING(alignment)) - (uintptr_t)extent_base_get(extent);
	assert(new_addr == NULL || leadsize == 0);
	assert(extent_size_get(extent) >= leadsize + size);
	trailsize = extent_size_get(extent) - leadsize - size;
	if (extent_zeroed_get(extent))
		*zero = true;
	if (extent_committed_get(extent))
		*commit = true;

	/* Split the lead. */
	if (leadsize != 0) {
		extent_t *lead = extent;
		extent = extent_split_wrapper(tsdn, arena, r_extent_hooks,
		    lead, leadsize, leadsize, size + trailsize, usize +
		    trailsize);
		if (extent == NULL) {
			extent_deregister(tsdn, lead);
			extent_leak(tsdn, arena, r_extent_hooks, cache, lead);
			if (!locked)
				malloc_mutex_unlock(tsdn, &arena->extents_mtx);
			return (NULL);
		}
		extent_heaps_insert(tsdn, extent_heaps, lead);
		arena_extent_cache_maybe_insert(tsdn, arena, lead, cache);
	}

	/* Split the trail. */
	if (trailsize != 0) {
		extent_t *trail = extent_split_wrapper(tsdn, arena,
		    r_extent_hooks, extent, size, usize, trailsize, trailsize);
		if (trail == NULL) {
			extent_deregister(tsdn, extent);
			extent_leak(tsdn, arena, r_extent_hooks, cache,
			    extent);
			if (!locked)
				malloc_mutex_unlock(tsdn, &arena->extents_mtx);
			return (NULL);
		}
		extent_heaps_insert(tsdn, extent_heaps, trail);
		arena_extent_cache_maybe_insert(tsdn, arena, trail, cache);
	} else if (leadsize == 0) {
		/*
		 * Splitting causes usize to be set as a side effect, but no
		 * splitting occurred.
		 */
		extent_usize_set(extent, usize);
	}

	if (commit && !extent_committed_get(extent) &&
	    extent_commit_wrapper(tsdn, arena, r_extent_hooks, extent, 0,
	    extent_size_get(extent))) {
		if (!locked)
			malloc_mutex_unlock(tsdn, &arena->extents_mtx);
		extent_record(tsdn, arena, r_extent_hooks, extent_heaps, cache,
		    extent);
		return (NULL);
	}

	if (pad != 0)
		extent_addr_randomize(tsdn, extent, alignment);
	extent_active_set(extent, true);
	if (slab) {
		extent_slab_set(extent, slab);
		extent_interior_register(tsdn, rtree_ctx, extent);
	}

	if (!locked)
		malloc_mutex_unlock(tsdn, &arena->extents_mtx);

	if (*zero) {
		if (!extent_zeroed_get(extent)) {
			memset(extent_addr_get(extent), 0,
			    extent_usize_get(extent));
		} else if (config_debug) {
			size_t i;
			size_t *p = (size_t *)(uintptr_t)
			    extent_addr_get(extent);

			for (i = 0; i < usize / sizeof(size_t); i++)
				assert(p[i] == 0);
		}
	}
	return (extent);
}

/*
 * If the caller specifies (!*zero), it is still possible to receive zeroed
 * memory, in which case *zero is toggled to true.  arena_extent_alloc() takes
 * advantage of this to avoid demanding zeroed extents, but taking advantage of
 * them if they are returned.
 */
static void *
extent_alloc_core(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec)
{
	void *ret;

	assert(size != 0);
	assert(alignment != 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);
	/* mmap. */
	if ((ret = extent_alloc_mmap(new_addr, size, alignment, zero, commit))
	    != NULL)
		return (ret);
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);

	/* All strategies for allocation failed. */
	return (NULL);
}

static extent_t *
extent_alloc_cache_impl(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, bool locked, void *new_addr, size_t usize,
    size_t pad, size_t alignment, bool *zero, bool *commit, bool slab)
{
	extent_t *extent;

	assert(usize + pad != 0);
	assert(alignment != 0);

	extent = extent_recycle(tsdn, arena, r_extent_hooks,
	    arena->extents_cached, locked, true, new_addr, usize, pad,
	    alignment, zero, commit, slab);
	if (extent == NULL)
		return (NULL);
	return (extent);
}

extent_t *
extent_alloc_cache_locked(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{

	malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);

	return (extent_alloc_cache_impl(tsdn, arena, r_extent_hooks, true,
	    new_addr, usize, pad, alignment, zero, commit, slab));
}

extent_t *
extent_alloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{

	return (extent_alloc_cache_impl(tsdn, arena, r_extent_hooks, false,
	    new_addr, usize, pad, alignment, zero, commit, slab));
}

static void *
extent_alloc_default_impl(tsdn_t *tsdn, arena_t *arena, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit)
{
	void *ret;

	ret = extent_alloc_core(tsdn, arena, new_addr, size, alignment, zero,
	    commit, arena->dss_prec);
	if (ret == NULL)
		return (NULL);

	return (ret);
}

static void *
extent_alloc_default(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind)
{
	tsdn_t *tsdn;
	arena_t *arena;

	assert(extent_hooks == &extent_hooks_default);

	tsdn = tsdn_fetch();
	arena = arena_get(tsdn, arena_ind, false);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);

	return (extent_alloc_default_impl(tsdn, arena, new_addr, size,
	    alignment, zero, commit));
}

static extent_t *
extent_alloc_retained(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{
	extent_t *extent;

	assert(usize != 0);
	assert(alignment != 0);

	extent = extent_recycle(tsdn, arena, r_extent_hooks,
	    arena->extents_retained, false, false, new_addr, usize, pad,
	    alignment, zero, commit, slab);
	if (extent != NULL && config_stats) {
		size_t size = usize + pad;
		arena->stats.retained -= size;
	}

	return (extent);
}

static extent_t *
extent_alloc_wrapper_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{
	extent_t *extent;
	size_t size;
	void *addr;

	size = usize + pad;
	extent = extent_alloc(tsdn, arena);
	if (extent == NULL)
		return (NULL);
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		addr = extent_alloc_default_impl(tsdn, arena, new_addr, size,
		    alignment, zero, commit);
	} else {
		addr = (*r_extent_hooks)->alloc(*r_extent_hooks, new_addr, size,
		    alignment, zero, commit, arena->ind);
	}
	if (addr == NULL) {
		extent_dalloc(tsdn, arena, extent);
		return (NULL);
	}
	extent_init(extent, arena, addr, size, usize, true, zero, commit, slab);
	if (pad != 0)
		extent_addr_randomize(tsdn, extent, alignment);
	if (extent_register(tsdn, extent)) {
		extent_leak(tsdn, arena, r_extent_hooks, false, extent);
		return (NULL);
	}

	return (extent);
}

extent_t *
extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{
	extent_t *extent;

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	extent = extent_alloc_retained(tsdn, arena, r_extent_hooks, new_addr,
	    usize, pad, alignment, zero, commit, slab);
	if (extent == NULL) {
		extent = extent_alloc_wrapper_hard(tsdn, arena, r_extent_hooks,
		    new_addr, usize, pad, alignment, zero, commit, slab);
	}

	return (extent);
}

static bool
extent_can_coalesce(const extent_t *a, const extent_t *b)
{

	if (extent_arena_get(a) != extent_arena_get(b))
		return (false);
	if (extent_active_get(a) != extent_active_get(b))
		return (false);
	if (extent_committed_get(a) != extent_committed_get(b))
		return (false);
	if (extent_retained_get(a) != extent_retained_get(b))
		return (false);

	return (true);
}

static void
extent_try_coalesce(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *a, extent_t *b,
    extent_heap_t extent_heaps[NPSIZES+1], bool cache)
{

	if (!extent_can_coalesce(a, b))
		return;

	extent_heaps_remove(tsdn, extent_heaps, a);
	extent_heaps_remove(tsdn, extent_heaps, b);

	arena_extent_cache_maybe_remove(tsdn, extent_arena_get(a), a, cache);
	arena_extent_cache_maybe_remove(tsdn, extent_arena_get(b), b, cache);

	if (extent_merge_wrapper(tsdn, arena, r_extent_hooks, a, b)) {
		extent_heaps_insert(tsdn, extent_heaps, a);
		extent_heaps_insert(tsdn, extent_heaps, b);
		arena_extent_cache_maybe_insert(tsdn, extent_arena_get(a), a,
		    cache);
		arena_extent_cache_maybe_insert(tsdn, extent_arena_get(b), b,
		    cache);
		return;
	}

	extent_heaps_insert(tsdn, extent_heaps, a);
	arena_extent_cache_maybe_insert(tsdn, extent_arena_get(a), a, cache);
}

static void
extent_record(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    extent_heap_t extent_heaps[NPSIZES+1], bool cache, extent_t *extent)
{
	extent_t *prev, *next;
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	assert(!cache || !extent_zeroed_get(extent));

	malloc_mutex_lock(tsdn, &arena->extents_mtx);
	extent_hooks_assure_initialized(arena, r_extent_hooks);

	extent_usize_set(extent, 0);
	extent_active_set(extent, false);
	extent_zeroed_set(extent, !cache && extent_zeroed_get(extent));
	if (extent_slab_get(extent)) {
		extent_interior_deregister(tsdn, rtree_ctx, extent);
		extent_slab_set(extent, false);
	}

	assert(extent_lookup(tsdn, extent_base_get(extent), true) == extent);
	extent_heaps_insert(tsdn, extent_heaps, extent);
	arena_extent_cache_maybe_insert(tsdn, arena, extent, cache);

	/* Try to coalesce forward. */
	next = rtree_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_past_get(extent), false);
	if (next != NULL) {
		extent_try_coalesce(tsdn, arena, r_extent_hooks, extent, next,
		    extent_heaps, cache);
	}

	/* Try to coalesce backward. */
	prev = rtree_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)extent_before_get(extent), false);
	if (prev != NULL) {
		extent_try_coalesce(tsdn, arena, r_extent_hooks, prev, extent,
		    extent_heaps, cache);
	}

	malloc_mutex_unlock(tsdn, &arena->extents_mtx);
}

void
extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, extent_t *extent)
{
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	if (extent_register(tsdn, extent)) {
		extent_leak(tsdn, arena, &extent_hooks, false, extent);
		return;
	}
	extent_dalloc_wrapper(tsdn, arena, &extent_hooks, extent);
}

void
extent_dalloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent)
{

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);

	extent_addr_set(extent, extent_base_get(extent));
	extent_zeroed_set(extent, false);

	extent_record(tsdn, arena, r_extent_hooks, arena->extents_cached, true,
	    extent);
}

static bool
extent_dalloc_default_impl(void *addr, size_t size)
{

	if (!have_dss || !extent_in_dss(addr))
		return (extent_dalloc_mmap(addr, size));
	return (true);
}


static bool
extent_dalloc_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);

	return (extent_dalloc_default_impl(addr, size));
}

void
extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent)
{
	bool err;

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);

	extent_addr_set(extent, extent_base_get(extent));

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	/*
	 * Try to deallocate.  Deregister first to avoid a race with other
	 * allocating threads, and reregister if deallocation fails.
	 */
	extent_deregister(tsdn, extent);
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		err = extent_dalloc_default_impl(extent_base_get(extent),
		    extent_size_get(extent));
	} else {
		err = (*r_extent_hooks)->dalloc(*r_extent_hooks,
		    extent_base_get(extent), extent_size_get(extent),
		    extent_committed_get(extent), arena->ind);
	}

	if (!err) {
		extent_dalloc(tsdn, arena, extent);
		return;
	}
	extent_reregister(tsdn, extent);
	/* Try to decommit; purge if that fails. */
	if (extent_committed_get(extent)) {
		extent_decommit_wrapper(tsdn, arena, r_extent_hooks, extent,
		    0, extent_size_get(extent));
	}
	extent_zeroed_set(extent, !extent_committed_get(extent) ||
	    !(*r_extent_hooks)->purge(*r_extent_hooks, extent_base_get(extent),
	    extent_size_get(extent), 0, extent_size_get(extent), arena->ind));

	if (config_stats)
		arena->stats.retained += extent_size_get(extent);

	extent_record(tsdn, arena, r_extent_hooks, arena->extents_retained,
	    false, extent);
}

static bool
extent_commit_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);

	return (pages_commit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length));
}

bool
extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length)
{
	bool err;

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	err = (*r_extent_hooks)->commit(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena->ind);
	extent_committed_set(extent, extent_committed_get(extent) || !err);
	return (err);
}

static bool
extent_decommit_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);

	return (pages_decommit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length));
}

bool
extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length)
{
	bool err;

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	err = (*r_extent_hooks)->decommit(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena->ind);
	extent_committed_set(extent, extent_committed_get(extent) && err);
	return (err);
}

static bool
extent_purge_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return (pages_purge((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length));
}

bool
extent_purge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length)
{

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	return ((*r_extent_hooks)->purge(*r_extent_hooks,
	    extent_base_get(extent), extent_size_get(extent), offset, length,
	    arena->ind));
}

static bool
extent_split_default(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);

	if (!maps_coalesce)
		return (true);
	return (false);
}

extent_t *
extent_split_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t size_a,
    size_t usize_a, size_t size_b, size_t usize_b)
{
	extent_t *trail;
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_elm_t *lead_elm_a, *lead_elm_b, *trail_elm_a, *trail_elm_b;

	assert(extent_size_get(extent) == size_a + size_b);

	extent_hooks_assure_initialized(arena, r_extent_hooks);

	trail = extent_alloc(tsdn, arena);
	if (trail == NULL)
		goto label_error_a;

	{
		extent_t lead;

		extent_init(&lead, arena, extent_addr_get(extent), size_a,
		    usize_a, extent_active_get(extent),
		    extent_zeroed_get(extent), extent_committed_get(extent),
		    extent_slab_get(extent));

		if (extent_rtree_acquire(tsdn, rtree_ctx, &lead, false, true,
		    &lead_elm_a, &lead_elm_b))
			goto label_error_b;
	}

	extent_init(trail, arena, (void *)((uintptr_t)extent_base_get(extent) +
	    size_a), size_b, usize_b, extent_active_get(extent),
	    extent_zeroed_get(extent), extent_committed_get(extent),
	    extent_slab_get(extent));
	if (extent_rtree_acquire(tsdn, rtree_ctx, trail, false, true,
	    &trail_elm_a, &trail_elm_b))
		goto label_error_c;

	if ((*r_extent_hooks)->split(*r_extent_hooks, extent_base_get(extent),
	    size_a + size_b, size_a, size_b, extent_committed_get(extent),
	    arena->ind))
		goto label_error_d;

	extent_size_set(extent, size_a);
	extent_usize_set(extent, usize_a);

	extent_rtree_write_acquired(tsdn, lead_elm_a, lead_elm_b, extent);
	extent_rtree_write_acquired(tsdn, trail_elm_a, trail_elm_b, trail);

	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
	extent_rtree_release(tsdn, trail_elm_a, trail_elm_b);

	return (trail);
label_error_d:
	extent_rtree_release(tsdn, trail_elm_a, trail_elm_b);
label_error_c:
	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
label_error_b:
	extent_dalloc(tsdn, arena, trail);
label_error_a:
	return (NULL);
}

static bool
extent_merge_default_impl(void *addr_a, void *addr_b)
{

	if (!maps_coalesce)
		return (true);
	if (have_dss && !extent_dss_mergeable(addr_a, addr_b))
		return (true);

	return (false);
}

static bool
extent_merge_default(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind)
{

	assert(extent_hooks == &extent_hooks_default);

	return (extent_merge_default_impl(addr_a, addr_b));
}

bool
extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *a, extent_t *b)
{
	bool err;
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_elm_t *a_elm_a, *a_elm_b, *b_elm_a, *b_elm_b;

	extent_hooks_assure_initialized(arena, r_extent_hooks);
	if (*r_extent_hooks == &extent_hooks_default) {
		/* Call directly to propagate tsdn. */
		err = extent_merge_default_impl(extent_base_get(a),
		    extent_base_get(b));
	} else {
		err = (*r_extent_hooks)->merge(*r_extent_hooks,
		    extent_base_get(a), extent_size_get(a), extent_base_get(b),
		    extent_size_get(b), extent_committed_get(a), arena->ind);
	}

	if (err)
		return (true);

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than extent_{,de}register() to do things in the right order.
	 */
	extent_rtree_acquire(tsdn, rtree_ctx, a, true, false, &a_elm_a,
	    &a_elm_b);
	extent_rtree_acquire(tsdn, rtree_ctx, b, true, false, &b_elm_a,
	    &b_elm_b);

	if (a_elm_b != NULL) {
		rtree_elm_write_acquired(tsdn, &extents_rtree, a_elm_b, NULL);
		rtree_elm_release(tsdn, &extents_rtree, a_elm_b);
	}
	if (b_elm_b != NULL) {
		rtree_elm_write_acquired(tsdn, &extents_rtree, b_elm_a, NULL);
		rtree_elm_release(tsdn, &extents_rtree, b_elm_a);
	} else
		b_elm_b = b_elm_a;

	extent_size_set(a, extent_size_get(a) + extent_size_get(b));
	extent_usize_set(a, extent_usize_get(a) + extent_usize_get(b));
	extent_zeroed_set(a, extent_zeroed_get(a) && extent_zeroed_get(b));

	extent_rtree_write_acquired(tsdn, a_elm_a, b_elm_b, a);
	extent_rtree_release(tsdn, a_elm_a, b_elm_b);

	extent_dalloc(tsdn, extent_arena_get(b), b);

	return (false);
}

bool
extent_boot(void)
{

	if (rtree_new(&extents_rtree, (unsigned)((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    LG_PAGE)))
		return (true);

	if (have_dss)
		extent_dss_boot();

	return (false);
}
