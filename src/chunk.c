#define	JEMALLOC_CHUNK_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;
bool	opt_overcommit = true;

malloc_mutex_t	chunks_mtx;
chunk_stats_t	stats_chunks;

rtree_t		*chunks_rtree;

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;
size_t		map_bias;
size_t		arena_maxclass; /* Max size class for arenas. */

/******************************************************************************/

/*
 * If the caller specifies (*zero == false), it is still possible to receive
 * zeroed memory, in which case *zero is toggled to true.  arena_chunk_alloc()
 * takes advantage of this to avoid demanding zeroed chunks, but taking
 * advantage of them if they are returned.
 */
void *
chunk_alloc(size_t size, bool base, bool *zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	if (config_swap && swap_enabled) {
		ret = chunk_alloc_swap(size, zero);
		if (ret != NULL)
			goto RETURN;
	}

	if (swap_enabled == false || opt_overcommit) {
		if (config_dss) {
			ret = chunk_alloc_dss(size, zero);
			if (ret != NULL)
				goto RETURN;
		}
		ret = chunk_alloc_mmap(size);
		if (ret != NULL) {
			*zero = true;
			goto RETURN;
		}
	}

	/* All strategies for allocation failed. */
	ret = NULL;
RETURN:
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

	assert(CHUNK_ADDR2BASE(ret) == ret);
	return (ret);
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
		if (config_swap && swap_enabled && chunk_dealloc_swap(chunk,
		    size) == false)
			return;
		if (config_dss && chunk_dealloc_dss(chunk, size) == false)
			return;
		chunk_dealloc_mmap(chunk, size);
	}
}

bool
chunk_boot(void)
{

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (ZU(1) << opt_lg_chunk);
	assert(chunksize >= PAGE_SIZE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> PAGE_SHIFT);

	if (config_stats || config_prof) {
		if (malloc_mutex_init(&chunks_mtx))
			return (true);
		memset(&stats_chunks, 0, sizeof(chunk_stats_t));
	}
	if (config_swap && chunk_swap_boot())
		return (true);
	if (chunk_mmap_boot())
		return (true);
	if (config_dss && chunk_dss_boot())
		return (true);
	if (config_ivsalloc) {
		chunks_rtree = rtree_new((ZU(1) << (LG_SIZEOF_PTR+3)) -
		    opt_lg_chunk);
		if (chunks_rtree == NULL)
			return (true);
	}

	return (false);
}
