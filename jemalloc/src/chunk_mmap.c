#define	JEMALLOC_CHUNK_MMAP_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

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

static void	*pages_map(void *addr, size_t size);
static void	pages_unmap(void *addr, size_t size);
static void	*chunk_alloc_mmap_slow(size_t size, bool unaligned);

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
			char buf[STRERROR_BUF];

			strerror_r(errno, buf, sizeof(buf));
			malloc_write("<jemalloc>: Error in munmap(): ");
			malloc_write(buf);
			malloc_write("\n");
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
		malloc_write("<jemalloc>: Error in munmap(): ");
		malloc_write(buf);
		malloc_write("\n");
		if (opt_abort)
			abort();
	}
}

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

void *
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
	} else
		ret = chunk_alloc_mmap_slow(size, false);

	return (ret);
}

void
chunk_dealloc_mmap(void *chunk, size_t size)
{

	pages_unmap(chunk, size);
}
