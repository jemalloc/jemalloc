#define	JEMALLOC_BASE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

static malloc_mutex_t	base_mtx;
static size_t		base_extent_sn_next;
static extent_heap_t	base_avail[NSIZES];
static extent_t		*base_extents;
static size_t		base_allocated;
static size_t		base_resident;
static size_t		base_mapped;

/******************************************************************************/

static extent_t *
base_extent_try_alloc(tsdn_t *tsdn)
{
	extent_t *extent;

	malloc_mutex_assert_owner(tsdn, &base_mtx);

	if (base_extents == NULL)
		return (NULL);
	extent = base_extents;
	base_extents = *(extent_t **)extent;
	return (extent);
}

static void
base_extent_dalloc(tsdn_t *tsdn, extent_t *extent)
{

	malloc_mutex_assert_owner(tsdn, &base_mtx);

	*(extent_t **)extent = base_extents;
	base_extents = extent;
}

static void
base_extent_init(extent_t *extent, void *addr, size_t size)
{
	size_t sn = atomic_add_zu(&base_extent_sn_next, 1) - 1;

	extent_init(extent, NULL, addr, size, 0, sn, true, true, true, false);
}

static extent_t *
base_extent_alloc(tsdn_t *tsdn, size_t minsize)
{
	extent_t *extent;
	size_t esize, nsize;
	void *addr;

	malloc_mutex_assert_owner(tsdn, &base_mtx);
	assert(minsize != 0);
	extent = base_extent_try_alloc(tsdn);
	/* Allocate enough space to also carve an extent out if necessary. */
	nsize = (extent == NULL) ? CACHELINE_CEILING(sizeof(extent_t)) : 0;
	esize = PAGE_CEILING(minsize + nsize);
	/*
	 * Directly call extent_alloc_mmap() because it's critical to allocate
	 * untouched demand-zeroed virtual memory.
	 */
	{
		bool zero = true;
		bool commit = true;
		addr = extent_alloc_mmap(NULL, esize, PAGE, &zero, &commit);
	}
	if (addr == NULL) {
		if (extent != NULL)
			base_extent_dalloc(tsdn, extent);
		return (NULL);
	}
	base_mapped += esize;
	if (extent == NULL) {
		extent = (extent_t *)addr;
		addr = (void *)((uintptr_t)addr + nsize);
		esize -= nsize;
		if (config_stats) {
			base_allocated += nsize;
			base_resident += PAGE_CEILING(nsize);
		}
	}
	base_extent_init(extent, addr, esize);
	return (extent);
}

/*
 * base_alloc() guarantees demand-zeroed memory, in order to make multi-page
 * sparse data structures such as radix tree nodes efficient with respect to
 * physical memory usage.
 */
void *
base_alloc(tsdn_t *tsdn, size_t size)
{
	void *ret;
	size_t csize;
	szind_t i;
	extent_t *extent;

	/*
	 * Round size up to nearest multiple of the cacheline size, so that
	 * there is no chance of false cache line sharing.
	 */
	csize = CACHELINE_CEILING(size);

	extent = NULL;
	malloc_mutex_lock(tsdn, &base_mtx);
	for (i = size2index(csize); i < NSIZES; i++) {
		extent = extent_heap_remove_first(&base_avail[i]);
		if (extent != NULL) {
			/* Use existing space. */
			break;
		}
	}
	if (extent == NULL) {
		/* Try to allocate more space. */
		extent = base_extent_alloc(tsdn, csize);
	}
	if (extent == NULL) {
		ret = NULL;
		goto label_return;
	}

	ret = extent_addr_get(extent);
	if (extent_size_get(extent) > csize) {
		szind_t index_floor;

		extent_addr_set(extent, (void *)((uintptr_t)ret + csize));
		extent_size_set(extent, extent_size_get(extent) - csize);
		/*
		 * Compute the index for the largest size class that does not
		 * exceed extent's size.
		 */
		index_floor = size2index(extent_size_get(extent) + 1) - 1;
		extent_heap_insert(&base_avail[index_floor], extent);
	} else
		base_extent_dalloc(tsdn, extent);
	if (config_stats) {
		base_allocated += csize;
		/*
		 * Add one PAGE to base_resident for every page boundary that is
		 * crossed by the new allocation.
		 */
		base_resident += PAGE_CEILING((uintptr_t)ret + csize) -
		    PAGE_CEILING((uintptr_t)ret);
	}
label_return:
	malloc_mutex_unlock(tsdn, &base_mtx);
	return (ret);
}

void
base_stats_get(tsdn_t *tsdn, size_t *allocated, size_t *resident,
    size_t *mapped)
{

	malloc_mutex_lock(tsdn, &base_mtx);
	assert(base_allocated <= base_resident);
	assert(base_resident <= base_mapped);
	*allocated = base_allocated;
	*resident = base_resident;
	*mapped = base_mapped;
	malloc_mutex_unlock(tsdn, &base_mtx);
}

bool
base_boot(void)
{
	szind_t i;

	if (malloc_mutex_init(&base_mtx, "base", WITNESS_RANK_BASE))
		return (true);
	base_extent_sn_next = 0;
	for (i = 0; i < NSIZES; i++)
		extent_heap_new(&base_avail[i]);
	base_extents = NULL;

	return (false);
}

void
base_prefork(tsdn_t *tsdn)
{

	malloc_mutex_prefork(tsdn, &base_mtx);
}

void
base_postfork_parent(tsdn_t *tsdn)
{

	malloc_mutex_postfork_parent(tsdn, &base_mtx);
}

void
base_postfork_child(tsdn_t *tsdn)
{

	malloc_mutex_postfork_child(tsdn, &base_mtx);
}
