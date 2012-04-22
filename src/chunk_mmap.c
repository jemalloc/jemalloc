#define	JEMALLOC_CHUNK_MMAP_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	*pages_map(void *addr, size_t size);
static void	pages_unmap(void *addr, size_t size);
static void	*chunk_alloc_mmap_slow(size_t size, size_t alignment,
    bool *zero);

/******************************************************************************/

static void *
pages_map(void *addr, size_t size)
{
	void *ret;

#ifdef _WIN32
	/*
	 * If VirtualAlloc can't allocate at the given address when one is
	 * given, it fails and returns NULL.
	 */
	ret = VirtualAlloc(addr, size, MEM_COMMIT | MEM_RESERVE,
	    PAGE_READWRITE);
#else
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
#endif
	assert(ret == NULL || (addr == NULL && ret != addr)
	    || (addr != NULL && ret == addr));
	return (ret);
}

static void
pages_unmap(void *addr, size_t size)
{

#ifdef _WIN32
	if (VirtualFree(addr, 0, MEM_RELEASE) == 0)
#else
	if (munmap(addr, size) == -1)
#endif
	{
		char buf[BUFERROR_BUF];

		buferror(errno, buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in "
#ifdef _WIN32
		              "VirtualFree"
#else
		              "munmap"
#endif
		              "(): %s\n", buf);
		if (opt_abort)
			abort();
	}
}

static void *
pages_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size)
{
	void *ret = (void *)((uintptr_t)addr + leadsize);

	assert(alloc_size >= leadsize + size);
#ifdef _WIN32
	{
		void *new_addr;

		pages_unmap(addr, alloc_size);
		new_addr = pages_map(ret, size);
		if (new_addr == ret)
			return (ret);
		if (new_addr)
			pages_unmap(new_addr, size);
		return (NULL);
	}
#else
	{
		size_t trailsize = alloc_size - leadsize - size;

		if (leadsize != 0)
			pages_unmap(addr, leadsize);
		if (trailsize != 0)
			pages_unmap((void *)((uintptr_t)ret + size), trailsize);
		return (ret);
	}
#endif
}

void
pages_purge(void *addr, size_t length)
{

#ifdef _WIN32
	VirtualAlloc(addr, length, MEM_RESET, PAGE_READWRITE);
#else
#  ifdef JEMALLOC_PURGE_MADVISE_DONTNEED
#    define JEMALLOC_MADV_PURGE MADV_DONTNEED
#  elif defined(JEMALLOC_PURGE_MADVISE_FREE)
#    define JEMALLOC_MADV_PURGE MADV_FREE
#  else
#    error "No method defined for purging unused dirty pages."
#  endif
	madvise(addr, length, JEMALLOC_MADV_PURGE);
#endif
}

static void *
chunk_alloc_mmap_slow(size_t size, size_t alignment, bool *zero)
{
	void *ret, *pages;
	size_t alloc_size, leadsize;

	alloc_size = size + alignment - PAGE;
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	do {
		pages = pages_map(NULL, alloc_size);
		if (pages == NULL)
			return (NULL);
		leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment) -
		    (uintptr_t)pages;
		ret = pages_trim(pages, alloc_size, leadsize, size);
	} while (ret == NULL);

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
#ifdef _WIN32
		return (chunk_alloc_mmap_slow(size, alignment, zero));
#else
		/* Try to extend chunk boundary. */
		if (pages_map((void *)((uintptr_t)ret + size), chunksize -
		    offset) == NULL) {
			/*
			 * Extension failed.  Clean up, then fall back to the
			 * reliable-but-expensive method.
			 */
			pages_unmap(ret, size);
			return (chunk_alloc_mmap_slow(size, alignment, zero));
		} else {
			/* Clean up unneeded leading space. */
			pages_unmap(ret, chunksize - offset);
			ret = (void *)((uintptr_t)ret + (chunksize - offset));
		}
#endif
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
