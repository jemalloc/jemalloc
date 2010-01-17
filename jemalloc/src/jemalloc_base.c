#define	JEMALLOC_BASE_C_
#include "internal/jemalloc_internal.h"

#ifdef JEMALLOC_STATS
size_t	base_mapped;
#endif

malloc_mutex_t	base_mtx;

/*
 * Current pages that are being used for internal memory allocations.  These
 * pages are carved up in cacheline-size quanta, so that there is no chance of
 * false cache line sharing.
 */
static void		*base_pages;
static void		*base_next_addr;
static void		*base_past_addr; /* Addr immediately past base_pages. */
static extent_node_t	*base_nodes;

#ifdef JEMALLOC_DSS
static bool	base_pages_alloc_dss(size_t minsize);
#endif
static bool	base_pages_alloc_mmap(size_t minsize);
static bool	base_pages_alloc(size_t minsize);

#ifdef JEMALLOC_DSS
static bool
base_pages_alloc_dss(size_t minsize)
{

	/*
	 * Do special DSS allocation here, since base allocations don't need to
	 * be chunk-aligned.
	 */
	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		intptr_t incr;
		size_t csize = CHUNK_CEILING(minsize);

		do {
			/* Get the current end of the DSS. */
			dss_max = sbrk(0);

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.  Don't worry about
			 * dss_max not being chunk-aligned though.
			 */
			incr = (intptr_t)chunksize
			    - (intptr_t)CHUNK_ADDR2OFFSET(dss_max);
			assert(incr >= 0);
			if ((size_t)incr < minsize)
				incr += csize;

			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = (void *)((intptr_t)dss_prev + incr);
				base_pages = dss_prev;
				base_next_addr = base_pages;
				base_past_addr = dss_max;
#ifdef JEMALLOC_STATS
				base_mapped += incr;
#endif
				malloc_mutex_unlock(&dss_mtx);
				return (false);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (true);
}
#endif

static bool
base_pages_alloc_mmap(size_t minsize)
{
	size_t csize;

	assert(minsize != 0);
	csize = PAGE_CEILING(minsize);
	base_pages = pages_map(NULL, csize);
	if (base_pages == NULL)
		return (true);
	base_next_addr = base_pages;
	base_past_addr = (void *)((uintptr_t)base_pages + csize);
#ifdef JEMALLOC_STATS
	base_mapped += csize;
#endif

	return (false);
}

static bool
base_pages_alloc(size_t minsize)
{

#ifdef JEMALLOC_DSS
	if (base_pages_alloc_dss(minsize) == false)
		return (false);

	if (minsize != 0)
#endif
	{
		if (base_pages_alloc_mmap(minsize) == false)
			return (false);
	}

	return (true);
}

void *
base_alloc(size_t size)
{
	void *ret;
	size_t csize;

	/* Round size up to nearest multiple of the cacheline size. */
	csize = CACHELINE_CEILING(size);

	malloc_mutex_lock(&base_mtx);
	/* Make sure there's enough space for the allocation. */
	if ((uintptr_t)base_next_addr + csize > (uintptr_t)base_past_addr) {
		if (base_pages_alloc(csize)) {
			malloc_mutex_unlock(&base_mtx);
			return (NULL);
		}
	}
	/* Allocate. */
	ret = base_next_addr;
	base_next_addr = (void *)((uintptr_t)base_next_addr + csize);
	malloc_mutex_unlock(&base_mtx);

	return (ret);
}

extent_node_t *
base_node_alloc(void)
{
	extent_node_t *ret;

	malloc_mutex_lock(&base_mtx);
	if (base_nodes != NULL) {
		ret = base_nodes;
		base_nodes = *(extent_node_t **)ret;
		malloc_mutex_unlock(&base_mtx);
	} else {
		malloc_mutex_unlock(&base_mtx);
		ret = (extent_node_t *)base_alloc(sizeof(extent_node_t));
	}

	return (ret);
}

void
base_node_dealloc(extent_node_t *node)
{

	malloc_mutex_lock(&base_mtx);
	*(extent_node_t **)node = base_nodes;
	base_nodes = node;
	malloc_mutex_unlock(&base_mtx);
}

bool
base_boot(void)
{

#ifdef JEMALLOC_STATS
	base_mapped = 0;
#endif
#ifdef JEMALLOC_DSS
	/*
	 * Allocate a base chunk here, since it doesn't actually have to be
	 * chunk-aligned.  Doing this before allocating any other chunks allows
	 * the use of space that would otherwise be wasted.
	 */
	base_pages_alloc(0);
#endif
	base_nodes = NULL;
	if (malloc_mutex_init(&base_mtx))
		return (true);

	return (false);
}
