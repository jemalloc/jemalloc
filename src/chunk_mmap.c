#define	JEMALLOC_CHUNK_MMAP_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*pages_map(void *addr, size_t size);
static void	pages_unmap(void *addr, size_t size);
static void	*chunk_alloc_mmap_slow(size_t size, size_t alignment,
    bool unaligned, bool *zero);

/******************************************************************************/

static void *
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
			char buf[BUFERROR_BUF];

			buferror(errno, buf, sizeof(buf));
			malloc_printf("<jemalloc: Error in munmap(): %s\n",
			    buf);
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
		char buf[BUFERROR_BUF];

		buferror(errno, buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in munmap(): %s\n", buf);
		if (opt_abort)
			abort();
	}
}

void
pages_purge(void *addr, size_t length)
{

#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED
#  define JEMALLOC_MADV_PURGE MADV_DONTNEED
#elif defined(JEMALLOC_PURGE_MADVISE_FREE)
#  define JEMALLOC_MADV_PURGE MADV_FREE
#else
#  error "No method defined for purging unused dirty pages."
#endif
	madvise(addr, length, JEMALLOC_MADV_PURGE);
}

static void *
chunk_alloc_mmap_slow(size_t size, size_t alignment, bool unaligned, bool *zero)
{
	void *ret, *pages;
	size_t alloc_size, leadsize, trailsize;

	alloc_size = size + alignment - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	pages = pages_map(NULL, alloc_size);
	if (pages == NULL)
		return (NULL);
	leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment) -
	    (uintptr_t)pages;
	assert(alloc_size >= leadsize + size);
	trailsize = alloc_size - leadsize - size;
	ret = (void *)((uintptr_t)pages + leadsize);
	if (leadsize != 0) {
		/* Note that mmap() returned an unaligned mapping. */
		unaligned = true;
		pages_unmap(pages, leadsize);
	}
	if (trailsize != 0)
		pages_unmap((void *)((uintptr_t)ret + size), trailsize);

	assert(ret != NULL);
	*zero = true;
	return (ret);
}

void *
chunk_alloc_mmap(size_t size, size_t alignment, bool *zero)
{
	void *ret;
	size_t offset;

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
	 * requested address.  As such, repeatedly trying to extend unaligned
	 * mappings could result in an infinite loop, so if extension fails,
	 * immediately fall back to the reliable method of over-allocation
	 * followed by trimming.
	 */

	ret = pages_map(NULL, size);
	if (ret == NULL)
		return (NULL);

	offset = ALIGNMENT_ADDR2OFFSET(ret, alignment);
	if (offset != 0) {
		/* Try to extend chunk boundary. */
		if (pages_map((void *)((uintptr_t)ret + size), chunksize -
		    offset) == NULL) {
			/*
			 * Extension failed.  Clean up, then fall back to the
			 * reliable-but-expensive method.
			 */
			pages_unmap(ret, size);
			return (chunk_alloc_mmap_slow(size, alignment, true,
			    zero));
		} else {
			/* Clean up unneeded leading space. */
			pages_unmap(ret, chunksize - offset);
			ret = (void *)((uintptr_t)ret + (chunksize - offset));
		}
	}

	assert(ret != NULL);
	*zero = true;
	return (ret);
}

bool
chunk_dealloc_mmap(void *chunk, size_t size)
{

	if (config_munmap)
		pages_unmap(chunk, size);

	return (config_munmap == false);
}
