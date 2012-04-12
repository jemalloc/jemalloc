#define	JEMALLOC_CHUNK_DSS_C_
#include "jemalloc/internal/jemalloc_internal.h"
/******************************************************************************/
/* Data. */

#ifndef JEMALLOC_HAVE_SBRK
void *
sbrk(intptr_t increment)
{

	not_implemented();

	return (NULL);
}
#endif

/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
static malloc_mutex_t	dss_mtx;

/* Base address of the DSS. */
static void		*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
static void		*dss_prev;
/* Current upper limit on DSS addresses. */
static void		*dss_max;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	dss_chunks_szad;
static extent_tree_t	dss_chunks_ad;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*chunk_recycle_dss(size_t size, size_t alignment, bool *zero);
static extent_node_t *chunk_dealloc_dss_record(void *chunk, size_t size);

/******************************************************************************/

static void *
chunk_recycle_dss(size_t size, size_t alignment, bool *zero)
{
	void *ret;
	extent_node_t *node;
	extent_node_t key;
	size_t alloc_size, leadsize, trailsize;

	cassert(config_dss);

	alloc_size = size + alignment - chunksize;
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	key.addr = NULL;
	key.size = alloc_size;
	malloc_mutex_lock(&dss_mtx);
	node = extent_tree_szad_nsearch(&dss_chunks_szad, &key);
	if (node == NULL) {
		malloc_mutex_unlock(&dss_mtx);
		return (NULL);
	}
	leadsize = ALIGNMENT_CEILING((uintptr_t)node->addr, alignment) -
	    (uintptr_t)node->addr;
	assert(alloc_size >= leadsize + size);
	trailsize = alloc_size - leadsize - size;
	ret = (void *)((uintptr_t)node->addr + leadsize);
	/* Remove node from the tree. */
	extent_tree_szad_remove(&dss_chunks_szad, node);
	extent_tree_ad_remove(&dss_chunks_ad, node);
	if (leadsize != 0) {
		/* Insert the leading space as a smaller chunk. */
		node->size = leadsize;
		extent_tree_szad_insert(&dss_chunks_szad, node);
		extent_tree_ad_insert(&dss_chunks_ad, node);
		node = NULL;
	}
	if (trailsize != 0) {
		/* Insert the trailing space as a smaller chunk. */
		if (node == NULL) {
			/*
			 * An additional node is required, but
			 * base_node_alloc() can cause a new base chunk to be
			 * allocated.  Drop dss_mtx in order to avoid deadlock,
			 * and if node allocation fails, deallocate the result
			 * before returning an error.
			 */
			malloc_mutex_unlock(&dss_mtx);
			node = base_node_alloc();
			if (node == NULL) {
				chunk_dealloc_dss(ret, size);
				return (NULL);
			}
			malloc_mutex_lock(&dss_mtx);
		}
		node->addr = (void *)((uintptr_t)(ret) + size);
		node->size = trailsize;
		extent_tree_szad_insert(&dss_chunks_szad, node);
		extent_tree_ad_insert(&dss_chunks_ad, node);
		node = NULL;
	}
	malloc_mutex_unlock(&dss_mtx);

	if (node != NULL)
		base_node_dealloc(node);
	if (*zero)
		memset(ret, 0, size);
	return (ret);
}

void *
chunk_alloc_dss(size_t size, size_t alignment, bool *zero)
{
	void *ret;

	cassert(config_dss);
	assert(size > 0 && (size & chunksize_mask) == 0);
	assert(alignment > 0 && (alignment & chunksize_mask) == 0);

	ret = chunk_recycle_dss(size, alignment, zero);
	if (ret != NULL)
		return (ret);

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		size_t gap_size, cpad_size;
		void *cpad, *dss_next;
		intptr_t incr;

		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		do {
			/* Get the current end of the DSS. */
			dss_max = sbrk(0);
			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.
			 */
			gap_size = (chunksize - CHUNK_ADDR2OFFSET(dss_max)) &
			    chunksize_mask;
			/*
			 * Compute how much chunk-aligned pad space (if any) is
			 * necessary to satisfy alignment.  This space can be
			 * recycled for later use.
			 */
			cpad = (void *)((uintptr_t)dss_max + gap_size);
			ret = (void *)ALIGNMENT_CEILING((uintptr_t)dss_max,
			    alignment);
			cpad_size = (uintptr_t)ret - (uintptr_t)cpad;
			dss_next = (void *)((uintptr_t)ret + size);
			if ((uintptr_t)ret < (uintptr_t)dss_max ||
			    (uintptr_t)dss_next < (uintptr_t)dss_max) {
				/* Wrap-around. */
				malloc_mutex_unlock(&dss_mtx);
				return (NULL);
			}
			incr = gap_size + cpad_size + size;
			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = dss_next;
				malloc_mutex_unlock(&dss_mtx);
				if (cpad_size != 0)
					chunk_dealloc_dss(cpad, cpad_size);
				*zero = true;
				return (ret);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

static extent_node_t *
chunk_dealloc_dss_record(void *chunk, size_t size)
{
	extent_node_t *xnode, *node, *prev, key;

	cassert(config_dss);

	xnode = NULL;
	while (true) {
		key.addr = (void *)((uintptr_t)chunk + size);
		node = extent_tree_ad_nsearch(&dss_chunks_ad, &key);
		/* Try to coalesce forward. */
		if (node != NULL && node->addr == key.addr) {
			/*
			 * Coalesce chunk with the following address range.
			 * This does not change the position within
			 * dss_chunks_ad, so only remove/insert from/into
			 * dss_chunks_szad.
			 */
			extent_tree_szad_remove(&dss_chunks_szad, node);
			node->addr = chunk;
			node->size += size;
			extent_tree_szad_insert(&dss_chunks_szad, node);
			break;
		} else if (xnode == NULL) {
			/*
			 * It is possible that base_node_alloc() will cause a
			 * new base chunk to be allocated, so take care not to
			 * deadlock on dss_mtx, and recover if another thread
			 * deallocates an adjacent chunk while this one is busy
			 * allocating xnode.
			 */
			malloc_mutex_unlock(&dss_mtx);
			xnode = base_node_alloc();
			malloc_mutex_lock(&dss_mtx);
			if (xnode == NULL)
				return (NULL);
		} else {
			/* Coalescing forward failed, so insert a new node. */
			node = xnode;
			xnode = NULL;
			node->addr = chunk;
			node->size = size;
			extent_tree_ad_insert(&dss_chunks_ad, node);
			extent_tree_szad_insert(&dss_chunks_szad, node);
			break;
		}
	}
	/* Discard xnode if it ended up unused do to a race. */
	if (xnode != NULL)
		base_node_dealloc(xnode);

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(&dss_chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)prev->addr + prev->size) ==
	    chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within dss_chunks_ad, so only
		 * remove/insert node from/into dss_chunks_szad.
		 */
		extent_tree_szad_remove(&dss_chunks_szad, prev);
		extent_tree_ad_remove(&dss_chunks_ad, prev);

		extent_tree_szad_remove(&dss_chunks_szad, node);
		node->addr = prev->addr;
		node->size += prev->size;
		extent_tree_szad_insert(&dss_chunks_szad, node);

		base_node_dealloc(prev);
	}

	return (node);
}

bool
chunk_in_dss(void *chunk)
{
	bool ret;

	cassert(config_dss);

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max)
		ret = true;
	else
		ret = false;
	malloc_mutex_unlock(&dss_mtx);

	return (ret);
}

bool
chunk_dealloc_dss(void *chunk, size_t size)
{
	bool ret;

	cassert(config_dss);

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max) {
		extent_node_t *node;

		/* Try to coalesce with other unused chunks. */
		node = chunk_dealloc_dss_record(chunk, size);
		if (node != NULL) {
			chunk = node->addr;
			size = node->size;
		}

		/* Get the current end of the DSS. */
		dss_max = sbrk(0);

		/*
		 * Try to shrink the DSS if this chunk is at the end of the
		 * DSS.  The sbrk() call here is subject to a race condition
		 * with threads that use brk(2) or sbrk(2) directly, but the
		 * alternative would be to leak memory for the sake of poorly
		 * designed multi-threaded programs.
		 */
		if ((void *)((uintptr_t)chunk + size) == dss_max
		    && (dss_prev = sbrk(-(intptr_t)size)) == dss_max) {
			/* Success. */
			dss_max = (void *)((intptr_t)dss_prev - (intptr_t)size);

			if (node != NULL) {
				extent_tree_szad_remove(&dss_chunks_szad, node);
				extent_tree_ad_remove(&dss_chunks_ad, node);
				base_node_dealloc(node);
			}
		} else
			madvise(chunk, size, MADV_DONTNEED);

		ret = false;
		goto label_return;
	}

	ret = true;
label_return:
	malloc_mutex_unlock(&dss_mtx);
	return (ret);
}

bool
chunk_dss_boot(void)
{

	cassert(config_dss);

	if (malloc_mutex_init(&dss_mtx))
		return (true);
	dss_base = sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;
	extent_tree_szad_new(&dss_chunks_szad);
	extent_tree_ad_new(&dss_chunks_ad);

	return (false);
}

void
chunk_dss_prefork(void)
{

	if (config_dss)
		malloc_mutex_prefork(&dss_mtx);
}

void
chunk_dss_postfork_parent(void)
{

	if (config_dss)
		malloc_mutex_postfork_parent(&dss_mtx);
}

void
chunk_dss_postfork_child(void)
{

	if (config_dss)
		malloc_mutex_postfork_child(&dss_mtx);
}

/******************************************************************************/
