#define	JEMALLOC_CHUNK_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;
#ifdef JEMALLOC_SWAP
bool	opt_overcommit = true;
#endif

#ifdef JEMALLOC_STATS
chunk_stats_t	stats_chunks;
#endif

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;
size_t		arena_chunk_header_npages;
size_t		arena_maxclass; /* Max size class for arenas. */

/******************************************************************************/

void *
chunk_alloc(size_t size, bool zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_SWAP
	if (swap_enabled) {
		ret = chunk_alloc_swap(size, zero);
		if (ret != NULL)
			goto RETURN;
	}

	if (swap_enabled == false || opt_overcommit) {
#endif
#ifdef JEMALLOC_DSS
		ret = chunk_alloc_dss(size, zero);
		if (ret != NULL)
			goto RETURN;
#endif
		ret = chunk_alloc_mmap(size);
		if (ret != NULL)
			goto RETURN;
#ifdef JEMALLOC_SWAP
	}
#endif

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

#ifdef JEMALLOC_SWAP
	if (swap_enabled && chunk_dealloc_swap(chunk, size) == false)
		return;
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

#ifdef JEMALLOC_SWAP
	if (chunk_swap_boot())
		return (true);
#endif
#ifdef JEMALLOC_DSS
	if (chunk_dss_boot())
		return (true);
#endif

	return (false);
}
