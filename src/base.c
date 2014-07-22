#define	JEMALLOC_BASE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static malloc_mutex_t	base_mtx;

/*
 * Current pages that are being used for internal memory allocations.  These
 * pages are carved up in cacheline-size quanta, so that there is no chance of
 * false cache line sharing.
 */
static void			*base_pages;
static void			*base_next_addr;
/* Addr immediately past base_pages. */
static void			*base_past_addr;
static extent_node_t		*base_nodes;
static arena_chunk_map_t	*base_mapelms;

/******************************************************************************/

static bool
base_pages_alloc(size_t minsize)
{
	size_t csize;

	assert(minsize != 0);
	csize = CHUNK_CEILING(minsize);
	base_pages = chunk_alloc_base(csize);
	if (base_pages == NULL)
		return (true);
	base_next_addr = base_pages;
	base_past_addr = (void *)((uintptr_t)base_pages + csize);

	return (false);
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
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, csize);

	return (ret);
}

void *
base_calloc(size_t number, size_t size)
{
	void *ret = base_alloc(number * size);

	if (ret != NULL)
		memset(ret, 0, number * size);

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
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret,
		    sizeof(extent_node_t));
	} else {
		malloc_mutex_unlock(&base_mtx);
		ret = (extent_node_t *)base_alloc(sizeof(extent_node_t));
	}

	return (ret);
}

void
base_node_dalloc(extent_node_t *node)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(node, sizeof(extent_node_t));
	malloc_mutex_lock(&base_mtx);
	*(extent_node_t **)node = base_nodes;
	base_nodes = node;
	malloc_mutex_unlock(&base_mtx);
}

arena_chunk_map_t *
base_mapelm_alloc(void)
{
	arena_chunk_map_t *ret;

	malloc_mutex_lock(&base_mtx);
	if (base_mapelms != NULL) {
		ret = base_mapelms;
		base_mapelms = *(arena_chunk_map_t **)ret;
		malloc_mutex_unlock(&base_mtx);
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret,
		    sizeof(arena_chunk_map_t));
	} else {
		malloc_mutex_unlock(&base_mtx);
		ret = (arena_chunk_map_t *)
		    base_alloc(sizeof(arena_chunk_map_t));
	}

	return (ret);
}

void
base_mapelm_dalloc(arena_chunk_map_t *mapelm)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(mapelm, sizeof(arena_chunk_map_t));
	malloc_mutex_lock(&base_mtx);
	*(arena_chunk_map_t **)mapelm = base_mapelms;
	base_mapelms = mapelm;
	malloc_mutex_unlock(&base_mtx);
}

bool
base_boot(void)
{

	base_nodes = NULL;
	base_mapelms = NULL;
	if (malloc_mutex_init(&base_mtx))
		return (true);

	return (false);
}

void
base_prefork(void)
{

	malloc_mutex_prefork(&base_mtx);
}

void
base_postfork_parent(void)
{

	malloc_mutex_postfork_parent(&base_mtx);
}

void
base_postfork_child(void)
{

	malloc_mutex_postfork_child(&base_mtx);
}
