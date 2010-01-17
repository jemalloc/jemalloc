#define	JEMALLOC_CHUNK_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;

#ifdef JEMALLOC_STATS
chunk_stats_t	stats_chunks;
#endif

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;
size_t		arena_chunk_header_npages;
size_t		arena_maxclass; /* Max size class for arenas. */

#ifdef JEMALLOC_DSS
malloc_mutex_t	dss_mtx;
void		*dss_base;
void		*dss_prev;
void		*dss_max;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	dss_chunks_szad;
static extent_tree_t	dss_chunks_ad;
#endif

/*
 * Used by chunk_alloc_mmap() to decide whether to attempt the fast path and
 * potentially avoid some system calls.  We can get away without TLS here,
 * since the state of mmap_unaligned only affects performance, rather than
 * correct function.
 */
static
#ifndef NO_TLS
       __thread
#endif
                bool	mmap_unaligned
#ifndef NO_TLS
                                       JEMALLOC_ATTR(tls_model("initial-exec"))
#endif
                                                                               ;
/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	pages_unmap(void *addr, size_t size);
#ifdef JEMALLOC_DSS
static void	*chunk_alloc_dss(size_t size);
static void	*chunk_recycle_dss(size_t size, bool zero);
#endif
static void	*chunk_alloc_mmap_slow(size_t size, bool unaligned);
static void	*chunk_alloc_mmap(size_t size);
#ifdef JEMALLOC_DSS
static extent_node_t *chunk_dealloc_dss_record(void *chunk, size_t size);
static bool	chunk_dealloc_dss(void *chunk, size_t size);
#endif
static void	chunk_dealloc_mmap(void *chunk, size_t size);

/******************************************************************************/

void *
pages_map(void *addr, size_t size)
{
	void *ret;

	/*
	 * We don't use MAP_FIXED here, because it can cause the *replacement*
	 * of existing mappings, and we only want to create new mappings.
	 */
	ret = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	assert(ret != NULL);

	if (ret == MAP_FAILED)
		ret = NULL;
	else if (addr != NULL && ret != addr) {
		/*
		 * We succeeded in mapping memory, but not in the right place.
		 */
		if (munmap(ret, size) == -1) {
			char buf[STRERROR_BUF];

			strerror_r(errno, buf, sizeof(buf));
			malloc_write4("<jemalloc>", ": Error in munmap(): ",
			    buf, "\n");
			if (opt_abort)
				abort();
		}
		ret = NULL;
	}

	assert(ret == NULL || (addr == NULL && ret != addr)
	    || (addr != NULL && ret == addr));
	return (ret);
}

static void
pages_unmap(void *addr, size_t size)
{

	if (munmap(addr, size) == -1) {
		char buf[STRERROR_BUF];

		strerror_r(errno, buf, sizeof(buf));
		malloc_write4("<jemalloc>", ": Error in munmap(): ", buf, "\n");
		if (opt_abort)
			abort();
	}
}

#ifdef JEMALLOC_DSS
static void *
chunk_alloc_dss(size_t size)
{

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		intptr_t incr;

		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		do {
			void *ret;

			/* Get the current end of the DSS. */
			dss_max = sbrk(0);

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.
			 */
			incr = (intptr_t)size
			    - (intptr_t)CHUNK_ADDR2OFFSET(dss_max);
			if (incr == (intptr_t)size)
				ret = dss_max;
			else {
				ret = (void *)((intptr_t)dss_max + incr);
				incr += size;
			}

			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = (void *)((intptr_t)dss_prev + incr);
				malloc_mutex_unlock(&dss_mtx);
				return (ret);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

static void *
chunk_recycle_dss(size_t size, bool zero)
{
	extent_node_t *node, key;

	key.addr = NULL;
	key.size = size;
	malloc_mutex_lock(&dss_mtx);
	node = extent_tree_szad_nsearch(&dss_chunks_szad, &key);
	if (node != NULL) {
		void *ret = node->addr;

		/* Remove node from the tree. */
		extent_tree_szad_remove(&dss_chunks_szad, node);
		if (node->size == size) {
			extent_tree_ad_remove(&dss_chunks_ad, node);
			base_node_dealloc(node);
		} else {
			/*
			 * Insert the remainder of node's address range as a
			 * smaller chunk.  Its position within dss_chunks_ad
			 * does not change.
			 */
			assert(node->size > size);
			node->addr = (void *)((uintptr_t)node->addr + size);
			node->size -= size;
			extent_tree_szad_insert(&dss_chunks_szad, node);
		}
		malloc_mutex_unlock(&dss_mtx);

		if (zero)
			memset(ret, 0, size);
		return (ret);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}
#endif

static void *
chunk_alloc_mmap_slow(size_t size, bool unaligned)
{
	void *ret;
	size_t offset;

	/* Beware size_t wrap-around. */
	if (size + chunksize <= size)
		return (NULL);

	ret = pages_map(NULL, size + chunksize);
	if (ret == NULL)
		return (NULL);

	/* Clean up unneeded leading/trailing space. */
	offset = CHUNK_ADDR2OFFSET(ret);
	if (offset != 0) {
		/* Note that mmap() returned an unaligned mapping. */
		unaligned = true;

		/* Leading space. */
		pages_unmap(ret, chunksize - offset);

		ret = (void *)((uintptr_t)ret +
		    (chunksize - offset));

		/* Trailing space. */
		pages_unmap((void *)((uintptr_t)ret + size),
		    offset);
	} else {
		/* Trailing space only. */
		pages_unmap((void *)((uintptr_t)ret + size),
		    chunksize);
	}

	/*
	 * If mmap() returned an aligned mapping, reset mmap_unaligned so that
	 * the next chunk_alloc_mmap() execution tries the fast allocation
	 * method.
	 */
	if (unaligned == false)
		mmap_unaligned = false;

	return (ret);
}

static void *
chunk_alloc_mmap(size_t size)
{
	void *ret;

	/*
	 * Ideally, there would be a way to specify alignment to mmap() (like
	 * NetBSD has), but in the absence of such a feature, we have to work
	 * hard to efficiently create aligned mappings.  The reliable, but
	 * slow method is to create a mapping that is over-sized, then trim the
	 * excess.  However, that always results in at least one call to
	 * pages_unmap().
	 *
	 * A more optimistic approach is to try mapping precisely the right
	 * amount, then try to append another mapping if alignment is off.  In
	 * practice, this works out well as long as the application is not
	 * interleaving mappings via direct mmap() calls.  If we do run into a
	 * situation where there is an interleaved mapping and we are unable to
	 * extend an unaligned mapping, our best option is to switch to the
	 * slow method until mmap() returns another aligned mapping.  This will
	 * tend to leave a gap in the memory map that is too small to cause
	 * later problems for the optimistic method.
	 *
	 * Another possible confounding factor is address space layout
	 * randomization (ASLR), which causes mmap(2) to disregard the
	 * requested address.  mmap_unaligned tracks whether the previous
	 * chunk_alloc_mmap() execution received any unaligned or relocated
	 * mappings, and if so, the current execution will immediately fall
	 * back to the slow method.  However, we keep track of whether the fast
	 * method would have succeeded, and if so, we make a note to try the
	 * fast method next time.
	 */

	if (mmap_unaligned == false) {
		size_t offset;

		ret = pages_map(NULL, size);
		if (ret == NULL)
			return (NULL);

		offset = CHUNK_ADDR2OFFSET(ret);
		if (offset != 0) {
			mmap_unaligned = true;
			/* Try to extend chunk boundary. */
			if (pages_map((void *)((uintptr_t)ret + size),
			    chunksize - offset) == NULL) {
				/*
				 * Extension failed.  Clean up, then revert to
				 * the reliable-but-expensive method.
				 */
				pages_unmap(ret, size);
				ret = chunk_alloc_mmap_slow(size, true);
			} else {
				/* Clean up unneeded leading space. */
				pages_unmap(ret, chunksize - offset);
				ret = (void *)((uintptr_t)ret + (chunksize -
				    offset));
			}
		}
	}
		ret = chunk_alloc_mmap_slow(size, false);

	return (ret);
}

void *
chunk_alloc(size_t size, bool zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_DSS
	ret = chunk_recycle_dss(size, zero);
	if (ret != NULL) {
		goto RETURN;
	}

	ret = chunk_alloc_dss(size);
	if (ret != NULL)
		goto RETURN;

#endif
	ret = chunk_alloc_mmap(size);
	if (ret != NULL)
		goto RETURN;

	/* All strategies for allocation failed. */
	ret = NULL;
RETURN:
#ifdef JEMALLOC_STATS
	if (ret != NULL) {
		stats_chunks.nchunks += (size / chunksize);
		stats_chunks.curchunks += (size / chunksize);
	}
	if (stats_chunks.curchunks > stats_chunks.highchunks)
		stats_chunks.highchunks = stats_chunks.curchunks;
#endif

	assert(CHUNK_ADDR2BASE(ret) == ret);
	return (ret);
}

#ifdef JEMALLOC_DSS
static extent_node_t *
chunk_dealloc_dss_record(void *chunk, size_t size)
{
	extent_node_t *node, *prev, key;

	key.addr = (void *)((uintptr_t)chunk + size);
	node = extent_tree_ad_nsearch(&dss_chunks_ad, &key);
	/* Try to coalesce forward. */
	if (node != NULL && node->addr == key.addr) {
		/*
		 * Coalesce chunk with the following address range.  This does
		 * not change the position within dss_chunks_ad, so only
		 * remove/insert from/into dss_chunks_szad.
		 */
		extent_tree_szad_remove(&dss_chunks_szad, node);
		node->addr = chunk;
		node->size += size;
		extent_tree_szad_insert(&dss_chunks_szad, node);
	} else {
		/*
		 * Coalescing forward failed, so insert a new node.  Drop
		 * dss_mtx during node allocation, since it is possible that a
		 * new base chunk will be allocated.
		 */
		malloc_mutex_unlock(&dss_mtx);
		node = base_node_alloc();
		malloc_mutex_lock(&dss_mtx);
		if (node == NULL)
			return (NULL);
		node->addr = chunk;
		node->size = size;
		extent_tree_ad_insert(&dss_chunks_ad, node);
		extent_tree_szad_insert(&dss_chunks_szad, node);
	}

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

static bool
chunk_dealloc_dss(void *chunk, size_t size)
{

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
			malloc_mutex_unlock(&dss_mtx);
		} else {
			malloc_mutex_unlock(&dss_mtx);
			madvise(chunk, size, MADV_DONTNEED);
		}

		return (false);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (true);
}
#endif

static void
chunk_dealloc_mmap(void *chunk, size_t size)
{

	pages_unmap(chunk, size);
}

void
chunk_dealloc(void *chunk, size_t size)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_STATS
	stats_chunks.curchunks -= (size / chunksize);
#endif

#ifdef JEMALLOC_DSS
	if (chunk_dealloc_dss(chunk, size) == false)
		return;

#endif
	chunk_dealloc_mmap(chunk, size);
}

bool
chunk_boot(void)
{

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (1LU << opt_lg_chunk);
	assert(chunksize >= PAGE_SIZE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> PAGE_SHIFT);

#ifdef JEMALLOC_STATS
	memset(&stats_chunks, 0, sizeof(chunk_stats_t));
#endif

#ifdef JEMALLOC_DSS
	if (malloc_mutex_init(&dss_mtx))
		return (true);
	dss_base = sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;
	extent_tree_szad_new(&dss_chunks_szad);
	extent_tree_ad_new(&dss_chunks_ad);
#endif

	return (false);
}
