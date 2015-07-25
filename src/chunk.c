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

/******************************************************************************/

bool
chunk_register(const void *chunk, const extent_node_t *node)
{

	assert(extent_node_addr_get(node) == chunk);

	if (rtree_set(&chunks_rtree, (uintptr_t)chunk, node))
		return (true);
	if (config_prof && opt_prof) {
		size_t size = extent_node_size_get(node);
		size_t nadd = (size == 0) ? 1 : size / chunksize;
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
			prof_gdump();
	}

	return (false);
}

void
chunk_deregister(const void *chunk, const extent_node_t *node)
{
	bool err;

	err = rtree_set(&chunks_rtree, (uintptr_t)chunk, NULL);
	assert(!err);
	if (config_prof && opt_prof) {
		size_t size = extent_node_size_get(node);
		size_t nsub = (size == 0) ? 1 : size / chunksize;
		assert(atomic_read_z(&curchunks) >= nsub);
		atomic_sub_z(&curchunks, nsub);
	}
}

/*
 * Do first-best-fit chunk selection, i.e. select the lowest chunk that best
 * fits.
 */
static extent_node_t *
chunk_first_best_fit(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, size_t size)
{
	extent_node_t key;

	assert(size == CHUNK_CEILING(size));

	extent_node_init(&key, arena, NULL, size, false);
	return (extent_tree_szad_nsearch(chunks_szad, &key));
}

static void *
chunk_recycle(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, bool cache, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool dalloc_node)
{
	void *ret;
	extent_node_t *node;
	size_t alloc_size, leadsize, trailsize;
	bool zeroed;

	assert(new_addr == NULL || alignment == chunksize);
	assert(dalloc_node || new_addr != NULL);

	alloc_size = CHUNK_CEILING(s2u(size + alignment - chunksize));
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	malloc_mutex_lock(&arena->chunks_mtx);
	if (new_addr != NULL) {
		extent_node_t key;
		extent_node_init(&key, arena, new_addr, alloc_size, false);
		node = extent_tree_ad_search(chunks_ad, &key);
	} else {
		node = chunk_first_best_fit(arena, chunks_szad, chunks_ad,
		    alloc_size);
	}
	if (node == NULL || (new_addr != NULL && extent_node_size_get(node) <
	    size)) {
		malloc_mutex_unlock(&arena->chunks_mtx);
		return (NULL);
	}
	leadsize = ALIGNMENT_CEILING((uintptr_t)extent_node_addr_get(node),
	    alignment) - (uintptr_t)extent_node_addr_get(node);
	assert(new_addr == NULL || leadsize == 0);
	assert(extent_node_size_get(node) >= leadsize + size);
	trailsize = extent_node_size_get(node) - leadsize - size;
	ret = (void *)((uintptr_t)extent_node_addr_get(node) + leadsize);
	zeroed = extent_node_zeroed_get(node);
	if (zeroed)
	    *zero = true;
	/* Remove node from the tree. */
	extent_tree_szad_remove(chunks_szad, node);
	extent_tree_ad_remove(chunks_ad, node);
	arena_chunk_cache_maybe_remove(arena, node, cache);
	if (leadsize != 0) {
		/* Insert the leading space as a smaller chunk. */
		extent_node_size_set(node, leadsize);
		extent_tree_szad_insert(chunks_szad, node);
		extent_tree_ad_insert(chunks_ad, node);
		arena_chunk_cache_maybe_insert(arena, node, cache);
		node = NULL;
	}
	if (trailsize != 0) {
		/* Insert the trailing space as a smaller chunk. */
		if (node == NULL) {
			node = arena_node_alloc(arena);
			if (node == NULL) {
				malloc_mutex_unlock(&arena->chunks_mtx);
				chunk_record(arena, chunks_szad, chunks_ad,
				    cache, ret, size, zeroed);
				return (NULL);
			}
		}
		extent_node_init(node, arena, (void *)((uintptr_t)(ret) + size),
		    trailsize, zeroed);
		extent_tree_szad_insert(chunks_szad, node);
		extent_tree_ad_insert(chunks_ad, node);
		arena_chunk_cache_maybe_insert(arena, node, cache);
		node = NULL;
	}
	malloc_mutex_unlock(&arena->chunks_mtx);

	assert(dalloc_node || node != NULL);
	if (dalloc_node && node != NULL)
		arena_node_dalloc(arena, node);
	if (*zero) {
		if (!zeroed)
			memset(ret, 0, size);
		else if (config_debug) {
			size_t i;
			size_t *p = (size_t *)(uintptr_t)ret;

			JEMALLOC_VALGRIND_MAKE_MEM_DEFINED(ret, size);
			for (i = 0; i < size / sizeof(size_t); i++)
				assert(p[i] == 0);
		}
	}
	return (ret);
}

static void *
chunk_alloc_core_dss(arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero)
{
	void *ret;

	if ((ret = chunk_recycle(arena, &arena->chunks_szad_dss,
	    &arena->chunks_ad_dss, false, new_addr, size, alignment, zero,
	    true)) != NULL)
		return (ret);
	ret = chunk_alloc_dss(arena, new_addr, size, alignment, zero);
	return (ret);
}

/*
 * If the caller specifies (!*zero), it is still possible to receive zeroed
 * memory, in which case *zero is toggled to true.  arena_chunk_alloc() takes
 * advantage of this to avoid demanding zeroed chunks, but taking advantage of
 * them if they are returned.
 */
static void *
chunk_alloc_core(arena_t *arena, void *new_addr, size_t size, size_t alignment,
    bool *zero, dss_prec_t dss_prec)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    chunk_alloc_core_dss(arena, new_addr, size, alignment, zero)) !=
	    NULL)
		return (ret);
	/* mmap. */
	if (!config_munmap && (ret = chunk_recycle(arena,
	    &arena->chunks_szad_mmap, &arena->chunks_ad_mmap, false, new_addr,
	    size, alignment, zero, true)) != NULL)
		return (ret);
	/*
	 * Requesting an address is not implemented for chunk_alloc_mmap(), so
	 * only call it if (new_addr == NULL).
	 */
	if (new_addr == NULL && (ret = chunk_alloc_mmap(size, alignment, zero))
	    != NULL)
		return (ret);
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    chunk_alloc_core_dss(arena, new_addr, size, alignment, zero)) !=
	    NULL)
		return (ret);

	/* All strategies for allocation failed. */
	return (NULL);
}

void *
chunk_alloc_base(size_t size)
{
	void *ret;
	bool zero;

	/*
	 * Directly call chunk_alloc_mmap() rather than chunk_alloc_core()
	 * because it's critical that chunk_alloc_base() return untouched
	 * demand-zeroed virtual memory.
	 */
	zero = true;
	ret = chunk_alloc_mmap(size, chunksize, &zero);
	if (ret == NULL)
		return (NULL);
	if (config_valgrind)
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);

	return (ret);
}

void *
chunk_alloc_cache(arena_t *arena, void *new_addr, size_t size, size_t alignment,
    bool *zero, bool dalloc_node)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	ret = chunk_recycle(arena, &arena->chunks_szad_cache,
	    &arena->chunks_ad_cache, true, new_addr, size, alignment, zero,
	    dalloc_node);
	if (ret == NULL)
		return (NULL);
	if (config_valgrind)
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
	return (ret);
}

static arena_t *
chunk_arena_get(unsigned arena_ind)
{
	arena_t *arena;

	/* Dodge tsd for a0 in order to avoid bootstrapping issues. */
	arena = (arena_ind == 0) ? a0get() : arena_get(tsd_fetch(), arena_ind,
	     false, true);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);
	return (arena);
}

static void *
chunk_alloc_arena(arena_t *arena, void *new_addr, size_t size, size_t alignment,
    bool *zero)
{
	void *ret;

	ret = chunk_alloc_core(arena, new_addr, size, alignment, zero,
	    arena->dss_prec);
	if (ret == NULL)
		return (NULL);
	if (config_valgrind)
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);

	return (ret);
}

/*
 * Default arena chunk allocation routine in the absence of user override.  This
 * function isn't actually used by jemalloc, but it does the right thing if the
 * application passes calls through to it during chunk allocation.
 */
void *
chunk_alloc_default(void *new_addr, size_t size, size_t alignment, bool *zero,
    unsigned arena_ind)
{
	arena_t *arena;

	arena = chunk_arena_get(arena_ind);
	return (chunk_alloc_arena(arena, new_addr, size, alignment, zero));
}

void *
chunk_alloc_wrapper(arena_t *arena, chunk_alloc_t *chunk_alloc, void *new_addr,
    size_t size, size_t alignment, bool *zero)
{
	void *ret;

	ret = chunk_alloc(new_addr, size, alignment, zero, arena->ind);
	if (ret == NULL)
		return (NULL);
	if (config_valgrind && chunk_alloc != chunk_alloc_default)
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, chunksize);
	return (ret);
}

void
chunk_record(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, bool cache, void *chunk, size_t size, bool zeroed)
{
	bool unzeroed;
	extent_node_t *node, *prev;
	extent_node_t key;

	assert(maps_coalesce || size == chunksize);
	assert(!cache || !zeroed);
	unzeroed = cache || !zeroed;
	JEMALLOC_VALGRIND_MAKE_MEM_NOACCESS(chunk, size);

	malloc_mutex_lock(&arena->chunks_mtx);
	extent_node_init(&key, arena, (void *)((uintptr_t)chunk + size), 0,
	    false);
	node = extent_tree_ad_nsearch(chunks_ad, &key);
	/* Try to coalesce forward. */
	if (node != NULL && extent_node_addr_get(node) ==
	    extent_node_addr_get(&key)) {
		/*
		 * Coalesce chunk with the following address range.  This does
		 * not change the position within chunks_ad, so only
		 * remove/insert from/into chunks_szad.
		 */
		extent_tree_szad_remove(chunks_szad, node);
		arena_chunk_cache_maybe_remove(arena, node, cache);
		extent_node_addr_set(node, chunk);
		extent_node_size_set(node, size + extent_node_size_get(node));
		extent_node_zeroed_set(node, extent_node_zeroed_get(node) &&
		    !unzeroed);
		extent_tree_szad_insert(chunks_szad, node);
		arena_chunk_cache_maybe_insert(arena, node, cache);
	} else {
		/* Coalescing forward failed, so insert a new node. */
		node = arena_node_alloc(arena);
		if (node == NULL) {
			/*
			 * Node allocation failed, which is an exceedingly
			 * unlikely failure.  Leak chunk after making sure its
			 * pages have already been purged, so that this is only
			 * a virtual memory leak.
			 */
			if (cache) {
				chunk_purge_wrapper(arena, arena->chunk_purge,
				    chunk, 0, size);
			}
			goto label_return;
		}
		extent_node_init(node, arena, chunk, size, !unzeroed);
		extent_tree_ad_insert(chunks_ad, node);
		extent_tree_szad_insert(chunks_szad, node);
		arena_chunk_cache_maybe_insert(arena, node, cache);
	}

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)extent_node_addr_get(prev) +
	    extent_node_size_get(prev)) == chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within chunks_ad, so only
		 * remove/insert node from/into chunks_szad.
		 */
		extent_tree_szad_remove(chunks_szad, prev);
		extent_tree_ad_remove(chunks_ad, prev);
		arena_chunk_cache_maybe_remove(arena, prev, cache);
		extent_tree_szad_remove(chunks_szad, node);
		arena_chunk_cache_maybe_remove(arena, node, cache);
		extent_node_addr_set(node, extent_node_addr_get(prev));
		extent_node_size_set(node, extent_node_size_get(prev) +
		    extent_node_size_get(node));
		extent_node_zeroed_set(node, extent_node_zeroed_get(prev) &&
		    extent_node_zeroed_get(node));
		extent_tree_szad_insert(chunks_szad, node);
		arena_chunk_cache_maybe_insert(arena, node, cache);

		arena_node_dalloc(arena, prev);
	}

label_return:
	malloc_mutex_unlock(&arena->chunks_mtx);
}

void
chunk_dalloc_cache(arena_t *arena, void *chunk, size_t size)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	if (!maps_coalesce && size != chunksize) {
		chunk_dalloc_arena(arena, chunk, size, false);
		return;
	}

	chunk_record(arena, &arena->chunks_szad_cache, &arena->chunks_ad_cache,
	    true, chunk, size, false);
	arena_maybe_purge(arena);
}

void
chunk_dalloc_arena(arena_t *arena, void *chunk, size_t size, bool zeroed)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	if (have_dss && chunk_in_dss(chunk)) {
		chunk_record(arena, &arena->chunks_szad_dss,
		    &arena->chunks_ad_dss, false, chunk, size, zeroed);
	} else if (chunk_dalloc_mmap(chunk, size)) {
		chunk_record(arena, &arena->chunks_szad_mmap,
		    &arena->chunks_ad_mmap, false, chunk, size, zeroed);
	}
}

/*
 * Default arena chunk deallocation routine in the absence of user override.
 * This function isn't actually used by jemalloc, but it does the right thing if
 * the application passes calls through to it during chunk deallocation.
 */
bool
chunk_dalloc_default(void *chunk, size_t size, unsigned arena_ind)
{

	chunk_dalloc_arena(chunk_arena_get(arena_ind), chunk, size, false);
	return (false);
}

void
chunk_dalloc_wrapper(arena_t *arena, chunk_dalloc_t *chunk_dalloc, void *chunk,
    size_t size)
{

	chunk_dalloc(chunk, size, arena->ind);
	if (config_valgrind && chunk_dalloc != chunk_dalloc_default)
		JEMALLOC_VALGRIND_MAKE_MEM_NOACCESS(chunk, size);
}

bool
chunk_purge_arena(arena_t *arena, void *chunk, size_t offset, size_t length)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return (pages_purge((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

bool
chunk_purge_default(void *chunk, size_t offset, size_t length,
    unsigned arena_ind)
{

	return (chunk_purge_arena(chunk_arena_get(arena_ind), chunk, offset,
	    length));
}

bool
chunk_purge_wrapper(arena_t *arena, chunk_purge_t *chunk_purge, void *chunk,
    size_t offset, size_t length)
{

	return (chunk_purge(chunk, offset, length, arena->ind));
}

static rtree_node_elm_t *
chunks_rtree_node_alloc(size_t nelms)
{

	return ((rtree_node_elm_t *)base_alloc(nelms *
	    sizeof(rtree_node_elm_t)));
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
		opt_lg_chunk = jemalloc_ffs((int)info.dwAllocationGranularity)
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
	if (rtree_new(&chunks_rtree, (ZU(1) << (LG_SIZEOF_PTR+3)) -
	    opt_lg_chunk, chunks_rtree_node_alloc, NULL))
		return (true);

	return (false);
}

void
chunk_prefork(void)
{

	chunk_dss_prefork();
}

void
chunk_postfork_parent(void)
{

	chunk_dss_postfork_parent();
}

void
chunk_postfork_child(void)
{

	chunk_dss_postfork_child();
}
