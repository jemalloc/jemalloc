#define	JEMALLOC_HUGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

typedef struct {
	malloc_mutex_t mtx;
	extent_tree_t tree;
} huge_map_t;

/*
 * Hash table holding trees of chunks that are stand-alone huge allocations. The
 * huge allocation pointers are used as keys. It provides fine-grained locking
 * by associating mutexes with individual trees rather than the entire map.
 */
static huge_map_t *huge_maps;
static size_t huge_size;

/* Retrieve the huge map corresponding to a huge allocation. */
static huge_map_t *get_huge_map(const void *ptr)
{
	size_t hash;

	/* Shift out the known bad bits, as we use the lower bits. */
	hash = (size_t)ptr >> opt_lg_chunk;

	/* Mix the remaining bits a bit. */
#if (LG_SIZEOF_PTR == 2)
	hash = hash_fmix_32(hash);
#elif (LG_SIZEOF_PTR == 3)
	hash = hash_fmix_64(hash);
#else
#error "not implemented"
#endif

	huge_map_t *huge = &huge_maps[hash & (huge_size - 1)];
	return huge;
}

void *
huge_malloc(tsd_t *tsd, arena_t *arena, size_t size, bool zero, bool try_tcache)
{
	size_t usize;

	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (NULL);
	}

	return (huge_palloc(tsd, arena, usize, chunksize, zero, try_tcache));
}

void *
huge_palloc(tsd_t *tsd, arena_t *arena, size_t usize, size_t alignment,
    bool zero, bool try_tcache)
{
	void *ret;
	size_t csize;
	extent_node_t *node;
	bool is_zeroed;
	huge_map_t *huge;

	/* Allocate one or more contiguous chunks for this request. */

	csize = CHUNK_CEILING(usize);
	assert(csize >= usize);

	/* Allocate an extent node with which to track the chunk. */
	node = ipalloct(tsd, CACHELINE_CEILING(sizeof(extent_node_t)),
	    CACHELINE, false, try_tcache, NULL);
	if (node == NULL)
		return (NULL);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	arena = arena_choose(tsd, arena);
	if (unlikely(arena == NULL)) {
		base_node_dalloc(node);
		return (NULL);
	}
	ret = arena_chunk_alloc_huge(arena, NULL, csize, alignment, &is_zeroed);
	if (ret == NULL) {
		idalloct(tsd, node, try_tcache);
		return (NULL);
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = usize;
	node->arena = arena;

	huge = get_huge_map(ret);

	malloc_mutex_lock(&huge->mtx);
	extent_tree_ad_insert(&huge->tree, node);
	malloc_mutex_unlock(&huge->mtx);

	if (config_fill && !zero) {
		if (unlikely(opt_junk))
			memset(ret, 0xa5, usize);
		else if (unlikely(opt_zero) && !is_zeroed)
			memset(ret, 0, usize);
	}

	return (ret);
}

#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk_impl)
#endif
static void
huge_dalloc_junk(void *ptr, size_t usize)
{

	if (config_fill && have_dss && unlikely(opt_junk)) {
		/*
		 * Only bother junk filling if the chunk isn't about to be
		 * unmapped.
		 */
		if (!config_munmap || (have_dss && chunk_in_dss(ptr)))
			memset(ptr, 0x5a, usize);
	}
}
#ifdef JEMALLOC_JET
#undef huge_dalloc_junk
#define	huge_dalloc_junk JEMALLOC_N(huge_dalloc_junk)
huge_dalloc_junk_t *huge_dalloc_junk = JEMALLOC_N(huge_dalloc_junk_impl);
#endif

static bool
huge_ralloc_no_move_expand(void *ptr, size_t oldsize, size_t size, bool zero) {
	size_t usize;
	void *expand_addr;
	size_t expand_size;
	extent_node_t *node, key;
	arena_t *arena;
	bool is_zeroed;
	void *ret;
	huge_map_t *huge;

	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (true);
	}

	expand_addr = ptr + CHUNK_CEILING(oldsize);
	expand_size = CHUNK_CEILING(usize) - CHUNK_CEILING(oldsize);
	assert(expand_size > 0);

	huge = get_huge_map(ptr);

	malloc_mutex_lock(&huge->mtx);

	key.addr = ptr;
	node = extent_tree_ad_search(&huge->tree, &key);
	assert(node != NULL);
	assert(node->addr == ptr);

	/* Find the current arena. */
	arena = node->arena;

	malloc_mutex_unlock(&huge->mtx);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	ret = arena_chunk_alloc_huge(arena, expand_addr, expand_size, chunksize,
				     &is_zeroed);
	if (ret == NULL)
		return (true);

	assert(ret == expand_addr);

	malloc_mutex_lock(&huge->mtx);
	/* Update the size of the huge allocation. */
	node->size = usize;
	malloc_mutex_unlock(&huge->mtx);

	if (config_fill && !zero) {
		if (unlikely(opt_junk))
			memset(ptr + oldsize, 0xa5, usize - oldsize);
		else if (unlikely(opt_zero) && !is_zeroed)
			memset(ptr + oldsize, 0, usize - oldsize);
	}
	return (false);
}

bool
huge_ralloc_no_move(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{
	size_t usize;
	huge_map_t *huge;

	/* Both allocations must be huge to avoid a move. */
	if (oldsize < chunksize)
		return (true);

	assert(s2u(oldsize) == oldsize);
	usize = s2u(size);
	if (usize == 0) {
		/* size_t overflow. */
		return (true);
	}

	huge = get_huge_map(ptr);

	/*
	 * Avoid moving the allocation if the existing chunk size accommodates
	 * the new size.
	 */
	if (CHUNK_CEILING(oldsize) >= CHUNK_CEILING(usize)
	    && CHUNK_CEILING(oldsize) <= CHUNK_CEILING(size+extra)) {
		size_t usize_next;

		/* Increase usize to incorporate extra. */
		while (usize < s2u(size+extra) && (usize_next = s2u(usize+1)) <
		    oldsize)
			usize = usize_next;

		/* Update the size of the huge allocation if it changed. */
		if (oldsize != usize) {
			extent_node_t *node, key;

			malloc_mutex_lock(&huge->mtx);

			key.addr = ptr;
			node = extent_tree_ad_search(&huge->tree, &key);
			assert(node != NULL);
			assert(node->addr == ptr);

			assert(node->size != usize);
			node->size = usize;

			malloc_mutex_unlock(&huge->mtx);

			if (oldsize < usize) {
				if (zero || (config_fill &&
				    unlikely(opt_zero))) {
					memset(ptr + oldsize, 0, usize -
					    oldsize);
				} else if (config_fill && unlikely(opt_junk)) {
					memset(ptr + oldsize, 0xa5, usize -
					    oldsize);
				}
			} else if (config_fill && unlikely(opt_junk) && oldsize
			    > usize)
				memset(ptr + usize, 0x5a, oldsize - usize);
		}
		return (false);
	}

	/* Shrink the allocation in-place. */
	if (CHUNK_CEILING(oldsize) >= CHUNK_CEILING(usize)) {
		extent_node_t *node, key;
		void *excess_addr;
		size_t excess_size;

		malloc_mutex_lock(&huge->mtx);

		key.addr = ptr;
		node = extent_tree_ad_search(&huge->tree, &key);
		assert(node != NULL);
		assert(node->addr == ptr);

		/* Update the size of the huge allocation. */
		node->size = usize;

		malloc_mutex_unlock(&huge->mtx);

		excess_addr = node->addr + CHUNK_CEILING(usize);
		excess_size = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);

		/* Zap the excess chunks. */
		huge_dalloc_junk(ptr + usize, oldsize - usize);
		if (excess_size > 0) {
			arena_chunk_dalloc_huge(node->arena, excess_addr,
			    excess_size);
		}

		return (false);
	}

	/* Attempt to expand the allocation in-place. */
	if (huge_ralloc_no_move_expand(ptr, oldsize, size + extra, zero)) {
		if (extra == 0)
			return (true);

		/* Try again, this time without extra. */
		return (huge_ralloc_no_move_expand(ptr, oldsize, size, zero));
	}
	return (false);
}

void *
huge_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize, size_t size,
    size_t extra, size_t alignment, bool zero, bool try_tcache_alloc,
    bool try_tcache_dalloc)
{
	void *ret;
	size_t copysize;

	/* Try to avoid moving the allocation. */
	if (!huge_ralloc_no_move(ptr, oldsize, size, extra, zero))
		return (ptr);

	/*
	 * size and oldsize are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	if (alignment > chunksize) {
		ret = huge_palloc(tsd, arena, size + extra, alignment, zero,
		    try_tcache_alloc);
	} else {
		ret = huge_malloc(tsd, arena, size + extra, zero,
		    try_tcache_alloc);
	}

	if (ret == NULL) {
		if (extra == 0)
			return (NULL);
		/* Try again, this time without extra. */
		if (alignment > chunksize) {
			ret = huge_palloc(tsd, arena, size, alignment, zero,
			    try_tcache_alloc);
		} else {
			ret = huge_malloc(tsd, arena, size, zero,
			    try_tcache_alloc);
		}

		if (ret == NULL)
			return (NULL);
	}

	/*
	 * Copy at most size bytes (not size+extra), since the caller has no
	 * expectation that the extra bytes will be reliably preserved.
	 */
	copysize = (size < oldsize) ? size : oldsize;
	memcpy(ret, ptr, copysize);
	iqalloc(tsd, ptr, try_tcache_dalloc);
	return (ret);
}

void
huge_dalloc(tsd_t *tsd, void *ptr, bool try_tcache)
{
	extent_node_t *node, key;
	huge_map_t *huge;

	huge = get_huge_map(ptr);

	malloc_mutex_lock(&huge->mtx);

	/* Extract from tree of huge allocations. */
	key.addr = ptr;
	node = extent_tree_ad_search(&huge->tree, &key);
	assert(node != NULL);
	assert(node->addr == ptr);
	extent_tree_ad_remove(&huge->tree, node);

	malloc_mutex_unlock(&huge->mtx);

	huge_dalloc_junk(node->addr, node->size);
	arena_chunk_dalloc_huge(node->arena, node->addr,
	    CHUNK_CEILING(node->size));
	idalloct(tsd, node, try_tcache);
}

size_t
huge_salloc(const void *ptr)
{
	size_t ret;
	extent_node_t *node, key;
	huge_map_t *huge;

	huge = get_huge_map(ptr);

	malloc_mutex_lock(&huge->mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge->tree, &key);
	assert(node != NULL);

	ret = node->size;

	malloc_mutex_unlock(&huge->mtx);

	return (ret);
}

prof_tctx_t *
huge_prof_tctx_get(const void *ptr)
{
	prof_tctx_t *ret;
	extent_node_t *node, key;
	huge_map_t *huge;

	huge = get_huge_map(ptr);

	malloc_mutex_lock(&huge->mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge->tree, &key);
	assert(node != NULL);

	ret = node->prof_tctx;

	malloc_mutex_unlock(&huge->mtx);

	return (ret);
}

void
huge_prof_tctx_set(const void *ptr, prof_tctx_t *tctx)
{
	extent_node_t *node, key;
	huge_map_t *huge;

	huge = get_huge_map(ptr);

	malloc_mutex_lock(&huge->mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge->tree, &key);
	assert(node != NULL);

	node->prof_tctx = tctx;

	malloc_mutex_unlock(&huge->mtx);
}

bool
huge_boot(unsigned ncpus)
{
	if (ncpus > 1)
		huge_size = pow2_ceil(ncpus * 16);
	else
		huge_size = 1;

	huge_maps = base_alloc(sizeof(huge_map_t) * huge_size);
	if (huge_maps == NULL)
		return (true);

	/* Initialize chunks data. */
	for (size_t i = 0; i < huge_size; i++) {
		if (malloc_mutex_init(&huge_maps[i].mtx))
			return (true);

		extent_tree_ad_new(&huge_maps[i].tree);
	}

	return (false);
}

void
huge_prefork(void)
{
	size_t i;

	for (i = 0; i < huge_size; i++)
		malloc_mutex_prefork(&huge_maps[i].mtx);
}

void
huge_postfork_parent(void)
{
	size_t i;

	for (i = 0; i < huge_size; i++)
		malloc_mutex_postfork_parent(&huge_maps[i].mtx);
}

void
huge_postfork_child(void)
{
	size_t i;

	for (i = 0; i < huge_size; i++)
		malloc_mutex_postfork_child(&huge_maps[i].mtx);
}
