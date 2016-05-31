#define	JEMALLOC_CHUNK_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

const char	*opt_dss = DSS_DEFAULT;
size_t		opt_lg_chunk = 0;

/* Used exclusively for gdump triggering. */
static size_t	curchunks;
static size_t	highchunks;

rtree_t		chunks_rtree;

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;

static void	*chunk_alloc_default(void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind);
static bool	chunk_dalloc_default(void *chunk, size_t size, bool committed,
    unsigned arena_ind);
static bool	chunk_commit_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_decommit_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_purge_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_split_default(void *chunk, size_t size, size_t size_a,
    size_t size_b, bool committed, unsigned arena_ind);
static bool	chunk_merge_default(void *chunk_a, size_t size_a, void *chunk_b,
    size_t size_b, bool committed, unsigned arena_ind);

const extent_hooks_t	extent_hooks_default = {
	chunk_alloc_default,
	chunk_dalloc_default,
	chunk_commit_default,
	chunk_decommit_default,
	chunk_purge_default,
	chunk_split_default,
	chunk_merge_default
};

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	chunk_record(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_heap_t extent_heaps[NPSIZES],
    bool cache, extent_t *extent);

/******************************************************************************/

static void
extent_heaps_insert(extent_heap_t extent_heaps[NPSIZES], extent_t *extent)
{
	size_t psz = extent_size_quantize_floor(extent_size_get(extent));
	pszind_t pind = psz2ind(psz);
	extent_heap_insert(&extent_heaps[pind], extent);
}

static void
extent_heaps_remove(extent_heap_t extent_heaps[NPSIZES], extent_t *extent)
{
	size_t psz = extent_size_quantize_floor(extent_size_get(extent));
	pszind_t pind = psz2ind(psz);
	extent_heap_remove(&extent_heaps[pind], extent);
}

static extent_hooks_t
extent_hooks_get_locked(arena_t *arena)
{

	return (arena->extent_hooks);
}

extent_hooks_t
extent_hooks_get(tsdn_t *tsdn, arena_t *arena)
{
	extent_hooks_t extent_hooks;

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	extent_hooks = extent_hooks_get_locked(arena);
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

	return (extent_hooks);
}

extent_hooks_t
extent_hooks_set(tsdn_t *tsdn, arena_t *arena,
    const extent_hooks_t *extent_hooks)
{
	extent_hooks_t old_extent_hooks;

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	old_extent_hooks = arena->extent_hooks;
	/*
	 * Copy each field atomically so that it is impossible for readers to
	 * see partially updated pointers.  There are places where readers only
	 * need one hook function pointer (therefore no need to copy the
	 * entirety of arena->extent_hooks), and stale reads do not affect
	 * correctness, so they perform unlocked reads.
	 */
#define	ATOMIC_COPY_HOOK(n) do {					\
	union {								\
		chunk_##n##_t	**n;					\
		void		**v;					\
	} u;								\
	u.n = &arena->extent_hooks.n;					\
	atomic_write_p(u.v, extent_hooks->n);				\
} while (0)
	ATOMIC_COPY_HOOK(alloc);
	ATOMIC_COPY_HOOK(dalloc);
	ATOMIC_COPY_HOOK(commit);
	ATOMIC_COPY_HOOK(decommit);
	ATOMIC_COPY_HOOK(purge);
	ATOMIC_COPY_HOOK(split);
	ATOMIC_COPY_HOOK(merge);
#undef ATOMIC_COPY_HOOK
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

	return (old_extent_hooks);
}

static void
extent_hooks_assure_initialized_impl(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, bool locked)
{
	static const extent_hooks_t uninitialized_hooks =
	    CHUNK_HOOKS_INITIALIZER;

	if (memcmp(extent_hooks, &uninitialized_hooks, sizeof(extent_hooks_t))
	    == 0) {
		*extent_hooks = locked ? extent_hooks_get_locked(arena) :
		    extent_hooks_get(tsdn, arena);
	}
}

static void
extent_hooks_assure_initialized_locked(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks)
{

	extent_hooks_assure_initialized_impl(tsdn, arena, extent_hooks, true);
}

static void
extent_hooks_assure_initialized(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks)
{

	extent_hooks_assure_initialized_impl(tsdn, arena, extent_hooks, false);
}

static bool
extent_rtree_acquire(tsdn_t *tsdn, const extent_t *extent, bool dependent,
    bool init_missing, rtree_elm_t **r_elm_a, rtree_elm_t **r_elm_b)
{

	*r_elm_a = rtree_elm_acquire(tsdn, &chunks_rtree,
	    (uintptr_t)extent_base_get(extent), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL)
		return (true);
	assert(*r_elm_a != NULL);

	if (extent_size_get(extent) > PAGE) {
		*r_elm_b = rtree_elm_acquire(tsdn, &chunks_rtree,
		    (uintptr_t)extent_last_get(extent), dependent,
		    init_missing);
		if (!dependent && *r_elm_b == NULL)
			return (true);
		assert(*r_elm_b != NULL);
	} else
		*r_elm_b = NULL;

	return (false);
}

static void
extent_rtree_write_acquired(tsdn_t *tsdn, rtree_elm_t *elm_a,
    rtree_elm_t *elm_b, const extent_t *extent)
{

	rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_a, extent);
	if (elm_b != NULL)
		rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_b, extent);
}

static void
extent_rtree_release(tsdn_t *tsdn, rtree_elm_t *elm_a, rtree_elm_t *elm_b)
{

	rtree_elm_release(tsdn, &chunks_rtree, elm_a);
	if (elm_b != NULL)
		rtree_elm_release(tsdn, &chunks_rtree, elm_b);
}

static void
chunk_interior_register(tsdn_t *tsdn, const extent_t *extent)
{
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &chunks_rtree,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE), extent);
	}
}

static bool
chunk_register(tsdn_t *tsdn, const extent_t *extent)
{
	rtree_elm_t *elm_a, *elm_b;

	if (extent_rtree_acquire(tsdn, extent, false, true, &elm_a, &elm_b))
		return (true);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, extent);
	if (extent_slab_get(extent))
		chunk_interior_register(tsdn, extent);
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof && opt_prof && extent_active_get(extent)) {
		size_t nadd = (extent_size_get(extent) == 0) ? 1 :
		    extent_size_get(extent) / chunksize;
		size_t cur = atomic_add_z(&curchunks, nadd);
		size_t high = atomic_read_z(&highchunks);
		while (cur > high && atomic_cas_z(&highchunks, high, cur)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highchunks update race.
			 */
			high = atomic_read_z(&highchunks);
		}
		if (cur > high && prof_gdump_get_unlocked())
			prof_gdump(tsdn);
	}

	return (false);
}

static void
chunk_interior_deregister(tsdn_t *tsdn, const extent_t *extent)
{
	size_t i;

	assert(extent_slab_get(extent));

	for (i = 1; i < (extent_size_get(extent) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &chunks_rtree,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

static void
chunk_deregister(tsdn_t *tsdn, const extent_t *extent)
{
	rtree_elm_t *elm_a, *elm_b;

	extent_rtree_acquire(tsdn, extent, true, false, &elm_a, &elm_b);
	extent_rtree_write_acquired(tsdn, elm_a, elm_b, NULL);
	if (extent_slab_get(extent))
		chunk_interior_deregister(tsdn, extent);
	extent_rtree_release(tsdn, elm_a, elm_b);

	if (config_prof && opt_prof && extent_active_get(extent)) {
		size_t nsub = (extent_size_get(extent) == 0) ? 1 :
		    extent_size_get(extent) / chunksize;
		assert(atomic_read_z(&curchunks) >= nsub);
		atomic_sub_z(&curchunks, nsub);
	}
}

/*
 * Do first-best-fit chunk selection, i.e. select the lowest chunk that best
 * fits.
 */
static extent_t *
chunk_first_best_fit(arena_t *arena, extent_heap_t extent_heaps[NPSIZES],
    size_t size)
{
	pszind_t pind, i;

	pind = psz2ind(extent_size_quantize_ceil(size));
	for (i = pind; i < NPSIZES; i++) {
		extent_t *extent = extent_heap_first(&extent_heaps[i]);
		if (extent != NULL)
			return (extent);
	}

	return (NULL);
}

static void
chunk_leak(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    bool cache, extent_t *extent)
{

	/*
	 * Leak chunk after making sure its pages have already been purged, so
	 * that this is only a virtual memory leak.
	 */
	if (cache) {
		chunk_purge_wrapper(tsdn, arena, extent_hooks, extent, 0,
		    extent_size_get(extent));
	}
	extent_dalloc(tsdn, arena, extent);
}

static extent_t *
chunk_recycle(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_heap_t extent_heaps[NPSIZES], bool cache, void *new_addr,
    size_t usize, size_t pad, size_t alignment, bool *zero, bool *commit,
    bool slab)
{
	extent_t *extent;
	size_t size, alloc_size, leadsize, trailsize;

	assert(new_addr == NULL || !slab);
	assert(pad == 0 || !slab);

	size = usize + pad;
	alloc_size = s2u(size + PAGE_CEILING(alignment) - PAGE);
	/* Beware size_t wrap-around. */
	if (alloc_size < usize)
		return (NULL);
	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	extent_hooks_assure_initialized_locked(tsdn, arena, extent_hooks);
	if (new_addr != NULL) {
		rtree_elm_t *elm;

		elm = rtree_elm_acquire(tsdn, &chunks_rtree,
		    (uintptr_t)new_addr, false, false);
		if (elm != NULL) {
			extent = rtree_elm_read_acquired(tsdn, &chunks_rtree,
			    elm);
			if (extent != NULL && (extent_active_get(extent) ||
			    extent_retained_get(extent) == cache))
				extent = NULL;
			rtree_elm_release(tsdn, &chunks_rtree, elm);
		} else
			extent = NULL;
	} else
		extent = chunk_first_best_fit(arena, extent_heaps, alloc_size);
	if (extent == NULL || (new_addr != NULL && extent_size_get(extent) <
	    size)) {
		malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
		return (NULL);
	}
	extent_heaps_remove(extent_heaps, extent);
	arena_chunk_cache_maybe_remove(arena, extent, cache);

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
		extent = chunk_split_wrapper(tsdn, arena, extent_hooks, lead,
		    leadsize, leadsize, size + trailsize, usize + trailsize);
		if (extent == NULL) {
			chunk_leak(tsdn, arena, extent_hooks, cache, lead);
			malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
			return (NULL);
		}
		extent_heaps_insert(extent_heaps, lead);
		arena_chunk_cache_maybe_insert(arena, lead, cache);
	}

	/* Split the trail. */
	if (trailsize != 0) {
		extent_t *trail = chunk_split_wrapper(tsdn, arena, extent_hooks,
		    extent, size, usize, trailsize, trailsize);
		if (trail == NULL) {
			chunk_leak(tsdn, arena, extent_hooks, cache, extent);
			malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
			return (NULL);
		}
		extent_heaps_insert(extent_heaps, trail);
		arena_chunk_cache_maybe_insert(arena, trail, cache);
	} else if (leadsize == 0) {
		/*
		 * Splitting causes usize to be set as a side effect, but no
		 * splitting occurred.
		 */
		extent_usize_set(extent, usize);
	}

	if (!extent_committed_get(extent) &&
	    extent_hooks->commit(extent_base_get(extent),
	    extent_size_get(extent), 0, extent_size_get(extent), arena->ind)) {
		malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
		chunk_record(tsdn, arena, extent_hooks, extent_heaps, cache,
		    extent);
		return (NULL);
	}

	if (pad != 0)
		extent_addr_randomize(tsdn, extent, alignment);
	extent_active_set(extent, true);
	if (slab) {
		extent_slab_set(extent, slab);
		chunk_interior_register(tsdn, extent);
	}

	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

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
 * memory, in which case *zero is toggled to true.  arena_chunk_alloc() takes
 * advantage of this to avoid demanding zeroed chunks, but taking advantage of
 * them if they are returned.
 */
static void *
chunk_alloc_core(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec)
{
	void *ret;

	assert(size != 0);
	assert(alignment != 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    chunk_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);
	/* mmap. */
	if ((ret = chunk_alloc_mmap(new_addr, size, alignment, zero, commit)) !=
	    NULL)
		return (ret);
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    chunk_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);

	/* All strategies for allocation failed. */
	return (NULL);
}

extent_t *
chunk_alloc_cache(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    void *new_addr, size_t usize, size_t pad, size_t alignment, bool *zero,
    bool slab)
{
	extent_t *extent;
	bool commit;

	assert(usize + pad != 0);
	assert(alignment != 0);

	commit = true;
	extent = chunk_recycle(tsdn, arena, extent_hooks, arena->chunks_cached,
	    true, new_addr, usize, pad, alignment, zero, &commit, slab);
	if (extent == NULL)
		return (NULL);
	assert(commit);
	return (extent);
}

static arena_t *
chunk_arena_get(tsdn_t *tsdn, unsigned arena_ind)
{
	arena_t *arena;

	arena = arena_get(tsdn, arena_ind, false);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);
	return (arena);
}

static void *
chunk_alloc_default(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit, unsigned arena_ind)
{
	void *ret;
	tsdn_t *tsdn;
	arena_t *arena;

	tsdn = tsdn_fetch();
	arena = chunk_arena_get(tsdn, arena_ind);
	ret = chunk_alloc_core(tsdn, arena, new_addr, size, alignment, zero,
	    commit, arena->dss_prec);
	if (ret == NULL)
		return (NULL);

	return (ret);
}

static extent_t *
chunk_alloc_retained(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    void *new_addr, size_t usize, size_t pad, size_t alignment, bool *zero,
    bool *commit, bool slab)
{
	extent_t *extent;

	assert(usize != 0);
	assert(alignment != 0);

	extent = chunk_recycle(tsdn, arena, extent_hooks,
	    arena->chunks_retained, false, new_addr, usize, pad, alignment,
	    zero, commit, slab);
	if (extent != NULL && config_stats) {
		size_t size = usize + pad;
		arena->stats.retained -= size;
	}

	return (extent);
}

static extent_t *
chunk_alloc_wrapper_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab)
{
	extent_t *extent;
	size_t size;
	void *addr;

	size = usize + pad;
	extent = extent_alloc(tsdn, arena);
	if (extent == NULL)
		return (NULL);
	addr = extent_hooks->alloc(new_addr, size, alignment, zero, commit,
	    arena->ind);
	if (addr == NULL) {
		extent_dalloc(tsdn, arena, extent);
		return (NULL);
	}
	extent_init(extent, arena, addr, size, usize, true, zero, commit, slab);
	if (pad != 0)
		extent_addr_randomize(tsdn, extent, alignment);
	if (chunk_register(tsdn, extent)) {
		chunk_leak(tsdn, arena, extent_hooks, false, extent);
		return (NULL);
	}

	return (extent);
}

extent_t *
chunk_alloc_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    void *new_addr, size_t usize, size_t pad, size_t alignment, bool *zero,
    bool *commit, bool slab)
{
	extent_t *extent;

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);

	extent = chunk_alloc_retained(tsdn, arena, extent_hooks, new_addr,
	    usize, pad, alignment, zero, commit, slab);
	if (extent == NULL) {
		extent = chunk_alloc_wrapper_hard(tsdn, arena, extent_hooks,
		    new_addr, usize, pad, alignment, zero, commit, slab);
	}

	return (extent);
}

static bool
chunk_can_coalesce(const extent_t *a, const extent_t *b)
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
chunk_try_coalesce(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *a, extent_t *b, extent_heap_t extent_heaps[NPSIZES], bool cache)
{

	if (!chunk_can_coalesce(a, b))
		return;

	extent_heaps_remove(extent_heaps, a);
	extent_heaps_remove(extent_heaps, b);

	arena_chunk_cache_maybe_remove(extent_arena_get(a), a, cache);
	arena_chunk_cache_maybe_remove(extent_arena_get(b), b, cache);

	if (chunk_merge_wrapper(tsdn, arena, extent_hooks, a, b)) {
		extent_heaps_insert(extent_heaps, a);
		extent_heaps_insert(extent_heaps, b);
		arena_chunk_cache_maybe_insert(extent_arena_get(a), a, cache);
		arena_chunk_cache_maybe_insert(extent_arena_get(b), b, cache);
		return;
	}

	extent_heaps_insert(extent_heaps, a);
	arena_chunk_cache_maybe_insert(extent_arena_get(a), a, cache);
}

static void
chunk_record(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_heap_t extent_heaps[NPSIZES], bool cache, extent_t *extent)
{
	extent_t *prev, *next;

	assert(!cache || !extent_zeroed_get(extent));

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	extent_hooks_assure_initialized_locked(tsdn, arena, extent_hooks);

	extent_usize_set(extent, 0);
	extent_active_set(extent, false);
	extent_zeroed_set(extent, !cache && extent_zeroed_get(extent));
	if (extent_slab_get(extent)) {
		chunk_interior_deregister(tsdn, extent);
		extent_slab_set(extent, false);
	}

	assert(chunk_lookup(tsdn, extent_base_get(extent), true) == extent);
	extent_heaps_insert(extent_heaps, extent);
	arena_chunk_cache_maybe_insert(arena, extent, cache);

	/* Try to coalesce forward. */
	next = rtree_read(tsdn, &chunks_rtree,
	    (uintptr_t)extent_past_get(extent), false);
	if (next != NULL) {
		chunk_try_coalesce(tsdn, arena, extent_hooks, extent, next,
		    extent_heaps, cache);
	}

	/* Try to coalesce backward. */
	prev = rtree_read(tsdn, &chunks_rtree,
	    (uintptr_t)extent_before_get(extent), false);
	if (prev != NULL) {
		chunk_try_coalesce(tsdn, arena, extent_hooks, prev, extent,
		    extent_heaps, cache);
	}

	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
}

void
chunk_dalloc_cache(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *extent)
{

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);

	extent_addr_set(extent, extent_base_get(extent));
	extent_zeroed_set(extent, false);

	chunk_record(tsdn, arena, extent_hooks, arena->chunks_cached, true,
	    extent);
}

static bool
chunk_dalloc_default(void *chunk, size_t size, bool committed,
    unsigned arena_ind)
{

	if (!have_dss || !chunk_in_dss(tsdn_fetch(), chunk))
		return (chunk_dalloc_mmap(chunk, size));
	return (true);
}

void
chunk_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *extent)
{

	assert(extent_base_get(extent) != NULL);
	assert(extent_size_get(extent) != 0);

	extent_addr_set(extent, extent_base_get(extent));

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);
	/* Try to deallocate. */
	if (!extent_hooks->dalloc(extent_base_get(extent),
	    extent_size_get(extent), extent_committed_get(extent),
	    arena->ind)) {
		chunk_deregister(tsdn, extent);
		extent_dalloc(tsdn, arena, extent);
		return;
	}
	/* Try to decommit; purge if that fails. */
	if (extent_committed_get(extent)) {
		extent_committed_set(extent,
		    extent_hooks->decommit(extent_base_get(extent),
		    extent_size_get(extent), 0, extent_size_get(extent),
		    arena->ind));
	}
	extent_zeroed_set(extent, !extent_committed_get(extent) ||
	    !extent_hooks->purge(extent_base_get(extent),
	    extent_size_get(extent), 0, extent_size_get(extent), arena->ind));

	if (config_stats)
		arena->stats.retained += extent_size_get(extent);

	chunk_record(tsdn, arena, extent_hooks, arena->chunks_retained, false,
	    extent);
}

static bool
chunk_commit_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	return (pages_commit((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

bool
chunk_commit_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *extent, size_t offset, size_t length)
{

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);
	return (extent_hooks->commit(extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena->ind));
}

static bool
chunk_decommit_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	return (pages_decommit((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

bool
chunk_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent, size_t offset,
    size_t length)
{

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);
	return (extent_hooks->decommit(extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena->ind));
}

static bool
chunk_purge_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	assert(chunk != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return (pages_purge((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

bool
chunk_purge_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *extent, size_t offset, size_t length)
{

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);
	return (extent_hooks->purge(extent_base_get(extent),
	    extent_size_get(extent), offset, length, arena->ind));
}

static bool
chunk_split_default(void *chunk, size_t size, size_t size_a, size_t size_b,
    bool committed, unsigned arena_ind)
{

	if (!maps_coalesce)
		return (true);
	return (false);
}

extent_t *
chunk_split_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
  extent_t *extent, size_t size_a, size_t usize_a, size_t size_b,
  size_t usize_b)
{
	extent_t *trail;
	rtree_elm_t *lead_elm_a, *lead_elm_b, *trail_elm_a, *trail_elm_b;

	assert(extent_size_get(extent) == size_a + size_b);

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);

	trail = extent_alloc(tsdn, arena);
	if (trail == NULL)
		goto label_error_a;

	{
		extent_t lead;

		extent_init(&lead, arena, extent_addr_get(extent), size_a,
		    usize_a, extent_active_get(extent),
		    extent_zeroed_get(extent), extent_committed_get(extent),
		    extent_slab_get(extent));

		if (extent_rtree_acquire(tsdn, &lead, false, true, &lead_elm_a,
		    &lead_elm_b))
			goto label_error_b;
	}

	extent_init(trail, arena, (void *)((uintptr_t)extent_base_get(extent) +
	    size_a), size_b, usize_b, extent_active_get(extent),
	    extent_zeroed_get(extent), extent_committed_get(extent),
	    extent_slab_get(extent));
	if (extent_rtree_acquire(tsdn, trail, false, true, &trail_elm_a,
	    &trail_elm_b))
		goto label_error_c;

	if (extent_hooks->split(extent_base_get(extent), size_a + size_b, size_a,
	    size_b, extent_committed_get(extent), arena->ind))
		goto label_error_d;

	extent_size_set(extent, size_a);
	extent_usize_set(extent, usize_a);

	extent_rtree_write_acquired(tsdn, lead_elm_a, lead_elm_b, extent);
	extent_rtree_write_acquired(tsdn, trail_elm_a, trail_elm_b, trail);

	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
	extent_rtree_release(tsdn, trail_elm_a, trail_elm_b);

	return (trail);
label_error_d:
	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
label_error_c:
	extent_rtree_release(tsdn, lead_elm_a, lead_elm_b);
label_error_b:
	extent_dalloc(tsdn, arena, trail);
label_error_a:
	return (NULL);
}

static bool
chunk_merge_default(void *chunk_a, size_t size_a, void *chunk_b, size_t size_b,
    bool committed, unsigned arena_ind)
{

	if (!maps_coalesce)
		return (true);
	if (have_dss) {
		tsdn_t *tsdn = tsdn_fetch();
		if (chunk_in_dss(tsdn, chunk_a) != chunk_in_dss(tsdn, chunk_b))
			return (true);
	}

	return (false);
}

bool
chunk_merge_wrapper(tsdn_t *tsdn, arena_t *arena, extent_hooks_t *extent_hooks,
    extent_t *a, extent_t *b)
{
	rtree_elm_t *a_elm_a, *a_elm_b, *b_elm_a, *b_elm_b;

	extent_hooks_assure_initialized(tsdn, arena, extent_hooks);
	if (extent_hooks->merge(extent_base_get(a), extent_size_get(a),
	    extent_base_get(b), extent_size_get(b), extent_committed_get(a),
	    arena->ind))
		return (true);

	/*
	 * The rtree writes must happen while all the relevant elements are
	 * owned, so the following code uses decomposed helper functions rather
	 * than chunk_{,de}register() to do things in the right order.
	 */
	extent_rtree_acquire(tsdn, a, true, false, &a_elm_a, &a_elm_b);
	extent_rtree_acquire(tsdn, b, true, false, &b_elm_a, &b_elm_b);

	if (a_elm_b != NULL) {
		rtree_elm_write_acquired(tsdn, &chunks_rtree, a_elm_b, NULL);
		rtree_elm_release(tsdn, &chunks_rtree, a_elm_b);
	}
	if (b_elm_b != NULL) {
		rtree_elm_write_acquired(tsdn, &chunks_rtree, b_elm_a, NULL);
		rtree_elm_release(tsdn, &chunks_rtree, b_elm_a);
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
chunk_boot(void)
{
#ifdef _WIN32
	SYSTEM_INFO info;
	GetSystemInfo(&info);

	/*
	 * Verify actual page size is equal to or an integral multiple of
	 * configured page size.
	 */
	if (info.dwPageSize & ((1U << LG_PAGE) - 1))
		return (true);

	/*
	 * Configure chunksize (if not set) to match granularity (usually 64K),
	 * so pages_map will always take fast path.
	 */
	if (!opt_lg_chunk) {
		opt_lg_chunk = ffs_u((unsigned)info.dwAllocationGranularity)
		    - 1;
	}
#else
	if (!opt_lg_chunk)
		opt_lg_chunk = LG_CHUNK_DEFAULT;
#endif

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (ZU(1) << opt_lg_chunk);
	assert(chunksize >= PAGE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> LG_PAGE);

	if (have_dss && chunk_dss_boot())
		return (true);
	if (rtree_new(&chunks_rtree, (unsigned)((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    LG_PAGE)))
		return (true);

	return (false);
}

void
chunk_prefork(tsdn_t *tsdn)
{

	chunk_dss_prefork(tsdn);
}

void
chunk_postfork_parent(tsdn_t *tsdn)
{

	chunk_dss_postfork_parent(tsdn);
}

void
chunk_postfork_child(tsdn_t *tsdn)
{

	chunk_dss_postfork_child(tsdn);
}
