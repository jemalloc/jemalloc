#define	JEMALLOC_CHUNK_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;

malloc_mutex_t	chunks_mtx;
chunk_stats_t	stats_chunks;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	chunks_szad;
static extent_tree_t	chunks_ad;

rtree_t		*chunks_rtree;

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;
size_t		map_bias;
size_t		arena_maxclass; /* Max size class for arenas. */

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*chunk_recycle(size_t size, size_t alignment, bool *zero);
static void	chunk_record(void *chunk, size_t size);

/******************************************************************************/

static void *
chunk_recycle(size_t size, size_t alignment, bool *zero)
{
	void *ret;
	extent_node_t *node;
	extent_node_t key;
	size_t alloc_size, leadsize, trailsize;

	alloc_size = size + alignment - chunksize;
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	key.addr = NULL;
	key.size = alloc_size;
	malloc_mutex_lock(&chunks_mtx);
	node = extent_tree_szad_nsearch(&chunks_szad, &key);
	if (node == NULL) {
		malloc_mutex_unlock(&chunks_mtx);
		return (NULL);
	}
	leadsize = ALIGNMENT_CEILING((uintptr_t)node->addr, alignment) -
	    (uintptr_t)node->addr;
	assert(alloc_size >= leadsize + size);
	trailsize = alloc_size - leadsize - size;
	ret = (void *)((uintptr_t)node->addr + leadsize);
	/* Remove node from the tree. */
	extent_tree_szad_remove(&chunks_szad, node);
	extent_tree_ad_remove(&chunks_ad, node);
	if (leadsize != 0) {
		/* Insert the leading space as a smaller chunk. */
		node->size = leadsize;
		extent_tree_szad_insert(&chunks_szad, node);
		extent_tree_ad_insert(&chunks_ad, node);
		node = NULL;
	}
	if (trailsize != 0) {
		/* Insert the trailing space as a smaller chunk. */
		if (node == NULL) {
			/*
			 * An additional node is required, but
			 * base_node_alloc() can cause a new base chunk to be
			 * allocated.  Drop chunks_mtx in order to avoid
			 * deadlock, and if node allocation fails, deallocate
			 * the result before returning an error.
			 */
			malloc_mutex_unlock(&chunks_mtx);
			node = base_node_alloc();
			if (node == NULL) {
				chunk_dealloc(ret, size, true);
				return (NULL);
			}
			malloc_mutex_lock(&chunks_mtx);
		}
		node->addr = (void *)((uintptr_t)(ret) + size);
		node->size = trailsize;
		extent_tree_szad_insert(&chunks_szad, node);
		extent_tree_ad_insert(&chunks_ad, node);
		node = NULL;
	}
	malloc_mutex_unlock(&chunks_mtx);

	if (node != NULL)
		base_node_dealloc(node);
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED
	/* Pages are zeroed as a side effect of pages_purge(). */
	*zero = true;
#else
	if (*zero) {
		VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
		memset(ret, 0, size);
	}
#endif
	return (ret);
}

/*
 * If the caller specifies (*zero == false), it is still possible to receive
 * zeroed memory, in which case *zero is toggled to true.  arena_chunk_alloc()
 * takes advantage of this to avoid demanding zeroed chunks, but taking
 * advantage of them if they are returned.
 */
void *
chunk_alloc(size_t size, size_t alignment, bool base, bool *zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert((alignment & chunksize_mask) == 0);

	ret = chunk_recycle(size, alignment, zero);
	if (ret != NULL)
		goto label_return;

	ret = chunk_alloc_mmap(size, alignment, zero);
	if (ret != NULL)
		goto label_return;

	if (config_dss) {
		ret = chunk_alloc_dss(size, alignment, zero);
		if (ret != NULL)
			goto label_return;
	}

	/* All strategies for allocation failed. */
	ret = NULL;
label_return:
	if (config_ivsalloc && base == false && ret != NULL) {
		if (rtree_set(chunks_rtree, (uintptr_t)ret, ret)) {
			chunk_dealloc(ret, size, true);
			return (NULL);
		}
	}
	if ((config_stats || config_prof) && ret != NULL) {
		bool gdump;
		malloc_mutex_lock(&chunks_mtx);
		if (config_stats)
			stats_chunks.nchunks += (size / chunksize);
		stats_chunks.curchunks += (size / chunksize);
		if (stats_chunks.curchunks > stats_chunks.highchunks) {
			stats_chunks.highchunks = stats_chunks.curchunks;
			if (config_prof)
				gdump = true;
		} else if (config_prof)
			gdump = false;
		malloc_mutex_unlock(&chunks_mtx);
		if (config_prof && opt_prof && opt_prof_gdump && gdump)
			prof_gdump();
	}
	if (config_debug && *zero && ret != NULL) {
		size_t i;
		size_t *p = (size_t *)(uintptr_t)ret;

		for (i = 0; i < size / sizeof(size_t); i++)
			assert(p[i] == 0);
	}
	assert(CHUNK_ADDR2BASE(ret) == ret);
	return (ret);
}

static void
chunk_record(void *chunk, size_t size)
{
	extent_node_t *xnode, *node, *prev, key;

	pages_purge(chunk, size);

	xnode = NULL;
	malloc_mutex_lock(&chunks_mtx);
	while (true) {
		key.addr = (void *)((uintptr_t)chunk + size);
		node = extent_tree_ad_nsearch(&chunks_ad, &key);
		/* Try to coalesce forward. */
		if (node != NULL && node->addr == key.addr) {
			/*
			 * Coalesce chunk with the following address range.
			 * This does not change the position within chunks_ad,
			 * so only remove/insert from/into chunks_szad.
			 */
			extent_tree_szad_remove(&chunks_szad, node);
			node->addr = chunk;
			node->size += size;
			extent_tree_szad_insert(&chunks_szad, node);
			break;
		} else if (xnode == NULL) {
			/*
			 * It is possible that base_node_alloc() will cause a
			 * new base chunk to be allocated, so take care not to
			 * deadlock on chunks_mtx, and recover if another thread
			 * deallocates an adjacent chunk while this one is busy
			 * allocating xnode.
			 */
			malloc_mutex_unlock(&chunks_mtx);
			xnode = base_node_alloc();
			if (xnode == NULL)
				return;
			malloc_mutex_lock(&chunks_mtx);
		} else {
			/* Coalescing forward failed, so insert a new node. */
			node = xnode;
			xnode = NULL;
			node->addr = chunk;
			node->size = size;
			extent_tree_ad_insert(&chunks_ad, node);
			extent_tree_szad_insert(&chunks_szad, node);
			break;
		}
	}
	/* Discard xnode if it ended up unused due to a race. */
	if (xnode != NULL)
		base_node_dealloc(xnode);

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(&chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)prev->addr + prev->size) ==
	    chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within chunks_ad, so only
		 * remove/insert node from/into chunks_szad.
		 */
		extent_tree_szad_remove(&chunks_szad, prev);
		extent_tree_ad_remove(&chunks_ad, prev);

		extent_tree_szad_remove(&chunks_szad, node);
		node->addr = prev->addr;
		node->size += prev->size;
		extent_tree_szad_insert(&chunks_szad, node);

		base_node_dealloc(prev);
	}
	malloc_mutex_unlock(&chunks_mtx);
}

void
chunk_dealloc(void *chunk, size_t size, bool unmap)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	if (config_ivsalloc)
		rtree_set(chunks_rtree, (uintptr_t)chunk, NULL);
	if (config_stats || config_prof) {
		malloc_mutex_lock(&chunks_mtx);
		stats_chunks.curchunks -= (size / chunksize);
		malloc_mutex_unlock(&chunks_mtx);
	}

	if (unmap) {
		if ((config_dss && chunk_in_dss(chunk)) ||
		    chunk_dealloc_mmap(chunk, size))
			chunk_record(chunk, size);
	}
}

bool
chunk_boot(void)
{

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (ZU(1) << opt_lg_chunk);
	assert(chunksize >= PAGE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> LG_PAGE);

	if (config_stats || config_prof) {
		if (malloc_mutex_init(&chunks_mtx))
			return (true);
		memset(&stats_chunks, 0, sizeof(chunk_stats_t));
	}
	if (config_dss && chunk_dss_boot())
		return (true);
	extent_tree_szad_new(&chunks_szad);
	extent_tree_ad_new(&chunks_ad);
	if (config_ivsalloc) {
		chunks_rtree = rtree_new((ZU(1) << (LG_SIZEOF_PTR+3)) -
		    opt_lg_chunk);
		if (chunks_rtree == NULL)
			return (true);
	}

	return (false);
}
