#define	JEMALLOC_HUGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

#ifdef JEMALLOC_STATS
uint64_t	huge_nmalloc;
uint64_t	huge_ndalloc;
size_t		huge_allocated;
#endif

malloc_mutex_t	huge_mtx;

/******************************************************************************/

/* Tree of chunks that are stand-alone huge allocations. */
static extent_tree_t	huge;

void *
huge_malloc(size_t size, bool zero)
{
	void *ret;
	size_t csize;
	extent_node_t *node;

	/* Allocate one or more contiguous chunks for this request. */

	csize = CHUNK_CEILING(size);
	if (csize == 0) {
		/* size is large enough to cause size_t wrap-around. */
		return (NULL);
	}

	/* Allocate an extent node with which to track the chunk. */
	node = base_node_alloc();
	if (node == NULL)
		return (NULL);

	ret = chunk_alloc(csize, &zero);
	if (ret == NULL) {
		base_node_dealloc(node);
		return (NULL);
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = csize;

	malloc_mutex_lock(&huge_mtx);
	extent_tree_ad_insert(&huge, node);
#ifdef JEMALLOC_STATS
	huge_nmalloc++;
	huge_allocated += csize;
#endif
	malloc_mutex_unlock(&huge_mtx);

#ifdef JEMALLOC_FILL
	if (zero == false) {
		if (opt_junk)
			memset(ret, 0xa5, csize);
		else if (opt_zero)
			memset(ret, 0, csize);
	}
#endif

	return (ret);
}

/* Only handles large allocations that require more than chunk alignment. */
void *
huge_palloc(size_t alignment, size_t size)
{
	void *ret;
	size_t alloc_size, chunk_size, offset;
	extent_node_t *node;
	bool zero;

	/*
	 * This allocation requires alignment that is even larger than chunk
	 * alignment.  This means that huge_malloc() isn't good enough.
	 *
	 * Allocate almost twice as many chunks as are demanded by the size or
	 * alignment, in order to assure the alignment can be achieved, then
	 * unmap leading and trailing chunks.
	 */
	assert(alignment >= chunksize);

	chunk_size = CHUNK_CEILING(size);

	if (size >= alignment)
		alloc_size = chunk_size + alignment - chunksize;
	else
		alloc_size = (alignment << 1) - chunksize;

	/* Allocate an extent node with which to track the chunk. */
	node = base_node_alloc();
	if (node == NULL)
		return (NULL);

	zero = false;
	ret = chunk_alloc(alloc_size, &zero);
	if (ret == NULL) {
		base_node_dealloc(node);
		return (NULL);
	}

	offset = (uintptr_t)ret & (alignment - 1);
	assert((offset & chunksize_mask) == 0);
	assert(offset < alloc_size);
	if (offset == 0) {
		/* Trim trailing space. */
		chunk_dealloc((void *)((uintptr_t)ret + chunk_size), alloc_size
		    - chunk_size);
	} else {
		size_t trailsize;

		/* Trim leading space. */
		chunk_dealloc(ret, alignment - offset);

		ret = (void *)((uintptr_t)ret + (alignment - offset));

		trailsize = alloc_size - (alignment - offset) - chunk_size;
		if (trailsize != 0) {
		    /* Trim trailing space. */
		    assert(trailsize < alloc_size);
		    chunk_dealloc((void *)((uintptr_t)ret + chunk_size),
			trailsize);
		}
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = chunk_size;

	malloc_mutex_lock(&huge_mtx);
	extent_tree_ad_insert(&huge, node);
#ifdef JEMALLOC_STATS
	huge_nmalloc++;
	huge_allocated += chunk_size;
#endif
	malloc_mutex_unlock(&huge_mtx);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ret, 0xa5, chunk_size);
	else if (opt_zero)
		memset(ret, 0, chunk_size);
#endif

	return (ret);
}

void *
huge_ralloc(void *ptr, size_t size, size_t oldsize)
{
	void *ret;
	size_t copysize;

	/* Avoid moving the allocation if the size class would not change. */
	if (oldsize > arena_maxclass &&
	    CHUNK_CEILING(size) == CHUNK_CEILING(oldsize)) {
#ifdef JEMALLOC_FILL
		if (opt_junk && size < oldsize) {
			memset((void *)((uintptr_t)ptr + size), 0x5a, oldsize
			    - size);
		} else if (opt_zero && size > oldsize) {
			memset((void *)((uintptr_t)ptr + oldsize), 0, size
			    - oldsize);
		}
#endif
		return (ptr);
	}

	/*
	 * If we get here, then size and oldsize are different enough that we
	 * need to use a different size class.  In that case, fall back to
	 * allocating new space and copying.
	 */
	ret = huge_malloc(size, false);
	if (ret == NULL)
		return (NULL);

	copysize = (size < oldsize) ? size : oldsize;
	memcpy(ret, ptr, copysize);
	idalloc(ptr);
	return (ret);
}

void
huge_dalloc(void *ptr)
{
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = ptr;
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);
	assert(node->addr == ptr);
	extent_tree_ad_remove(&huge, node);

#ifdef JEMALLOC_STATS
	huge_ndalloc++;
	huge_allocated -= node->size;
#endif

	malloc_mutex_unlock(&huge_mtx);

	/* Unmap chunk. */
#ifdef JEMALLOC_FILL
#if (defined(JEMALLOC_SWAP) || defined(JEMALLOC_DSS))
	if (opt_junk)
		memset(node->addr, 0x5a, node->size);
#endif
#endif
	chunk_dealloc(node->addr, node->size);

	base_node_dealloc(node);
}

size_t
huge_salloc(const void *ptr)
{
	size_t ret;
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	ret = node->size;

	malloc_mutex_unlock(&huge_mtx);

	return (ret);
}

#ifdef JEMALLOC_PROF
prof_thr_cnt_t *
huge_prof_cnt_get(const void *ptr)
{
	prof_thr_cnt_t *ret;
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	ret = node->prof_cnt;

	malloc_mutex_unlock(&huge_mtx);

	return (ret);
}

void
huge_prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt)
{
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = __DECONST(void *, ptr);
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);

	node->prof_cnt = cnt;

	malloc_mutex_unlock(&huge_mtx);
}
#endif

bool
huge_boot(void)
{

	/* Initialize chunks data. */
	if (malloc_mutex_init(&huge_mtx))
		return (true);
	extent_tree_ad_new(&huge);

#ifdef JEMALLOC_STATS
	huge_nmalloc = 0;
	huge_ndalloc = 0;
	huge_allocated = 0;
#endif

	return (false);
}
