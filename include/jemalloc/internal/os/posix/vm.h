#ifndef JEMALLOC_INTERNAL_OS_POSIX_VM_H
#define JEMALLOC_INTERNAL_OS_POSIX_VM_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/os/overcommit.h"

#include "jemalloc/internal/bit_util.h"

#include <sys/mman.h>
#if defined(JEMALLOC_HAVE_PRCTL) && defined(JEMALLOC_PAGEID)
#  include <sys/prctl.h>
#  ifndef PR_SET_VMA
#    define PR_SET_VMA 0x53564d41
#    define PR_SET_VMA_ANON_NAME 0
#  endif
#endif
#ifdef __NetBSD__
#  include <sys/bitops.h> /* ilog2 */
#endif

#ifdef JEMALLOC_HAVE_VM_MAKE_TAG
#  define PAGES_FD_TAG VM_MAKE_TAG(254U)
#else
#  define PAGES_FD_TAG -1
#endif
#define PAGES_PROT_COMMIT   (PROT_READ | PROT_WRITE)
#define PAGES_PROT_DECOMMIT (PROT_NONE)

#ifdef JEMALLOC_PAGEID
JEMALLOC_ALWAYS_INLINE int
os_page_id(void *addr, size_t size, const char *name) {
#  ifdef JEMALLOC_HAVE_PRCTL
	/*
	 * While parsing `/proc/<pid>/maps` file, the block could appear as
	 * 7f4836000000-7f4836800000 rw-p 00000000 00:00 0 [anon:jemalloc_pg_overcommit]`
	 */
	int n;
	assert(addr != NULL);
	n = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (uintptr_t)addr, size,
	    (uintptr_t)name);
	assert(n == 0 || (n == -1 && get_errno() == EINVAL));
	return n;
#  else
	return 0;
#  endif
}
#endif

JEMALLOC_ALWAYS_INLINE void *
os_vm_reserve(void *hint, size_t size, size_t alignment, bool *commit) {
	assert(ALIGNMENT_ADDR2BASE(hint, os_page) == hint);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	assert(size != 0);

	if (os_overcommits) {
		*commit = true;
	}

	void *ret;
	/*
	 * We don't use MAP_FIXED here, because it can cause the *replacement*
	 * of existing mappings, and we only want to create new mappings.
	 */
	{
		int flags = mmap_flags;
#ifdef __NetBSD__
		/*
		 * On NetBSD PAGE for a platform is defined to the
		 * maximum page size of all machine architectures
		 * for that platform, so that we can use the same
		 * binaries across all machine architectures.
		 */
		if (alignment > os_page || PAGE > os_page) {
			unsigned int a = ilog2(MAX(alignment, PAGE));
			flags |= MAP_ALIGNED(a);
		}
#endif
		int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;

		ret = mmap(hint, size, prot, flags, PAGES_FD_TAG, 0);
	}
	assert(ret != NULL);

	if (ret == MAP_FAILED) {
		ret = NULL;
	} else if (hint != NULL && ret != hint) {
		/*
		 * We succeeded in mapping memory, but not in the right place.
		 */
		os_vm_release(ret, size);
		ret = NULL;
	}
	assert(ret == NULL || (hint == NULL && ret != hint)
	    || (hint != NULL && ret == hint));
#ifdef JEMALLOC_PAGEID
	if (ret != NULL) {
		os_page_id(ret, size,
		    os_overcommits ? "jemalloc_pg_overcommit" : "jemalloc_pg");
	}
#endif
	return ret;
}

#if defined(__FreeBSD__) && defined(MAP_EXCL)
/*
 * FreeBSD has mechanisms both to mmap at a specific address without
 * touching existing mappings (MAP_EXCL), and to mmap with a specific
 * alignment.
 */
#  define OS_VM_HAS_FIXED_ALIGNED_RESERVE

JEMALLOC_ALWAYS_INLINE void *
os_vm_reserve_aligned_fixed(void *addr, size_t size, size_t alignment,
    bool *commit) {
	if (os_overcommits) {
		*commit = true;
	}

	int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
	int flags = mmap_flags;

	if (addr != NULL) {
		flags |= MAP_FIXED | MAP_EXCL;
	} else {
		unsigned alignment_bits = ffs_zu(alignment);
		assert(alignment_bits > 0);
		flags |= MAP_ALIGNED(alignment_bits);
	}

	void *ret = mmap(addr, size, prot, flags, -1, 0);
	if (ret == MAP_FAILED) {
		ret = NULL;
	}

	return ret;
}
#endif

JEMALLOC_ALWAYS_INLINE void
os_vm_release(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);

	if (munmap(addr, size) == -1) {
		char buf[BUFERROR_BUF];

		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in munmap(): %s\n", buf);
		if (opt_abort) {
			abort();
		}
	}
}

JEMALLOC_ALWAYS_INLINE void *
os_vm_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size,
    bool *commit) {
	assert(alloc_size >= leadsize + size);
	void  *ret = (void *)((byte_t *)addr + leadsize);
	size_t trailsize = alloc_size - leadsize - size;

	(void)commit;
	if (leadsize != 0) {
		os_vm_release(addr, leadsize);
	}
	if (trailsize != 0) {
		os_vm_release((void *)((byte_t *)ret + size), trailsize);
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_commit_impl(void *addr, size_t size, bool commit) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	int   prot = commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
	void *result = mmap(
	    addr, size, prot, mmap_flags | MAP_FIXED, PAGES_FD_TAG, 0);
	if (result == MAP_FAILED) {
		return true;
	}
	if (result != addr) {
		/*
		 * We succeeded in mapping memory, but not in the right
		 * place.
		 */
		os_vm_release(result, size);
		return true;
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_commit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, true);
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_decommit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, false);
}

JEMALLOC_ALWAYS_INLINE void
os_vm_mark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
#ifdef JEMALLOC_HAVE_MPROTECT
	if (head != NULL) {
		mprotect(head, PAGE, PROT_NONE);
	}
	if (tail != NULL) {
		mprotect(tail, PAGE, PROT_NONE);
	}
#else
	/* Decommit sets the region to PROT_NONE. */
	if (head != NULL) {
		os_vm_decommit(head, PAGE);
	}
	if (tail != NULL) {
		os_vm_decommit(tail, PAGE);
	}
#endif
}

JEMALLOC_ALWAYS_INLINE void
os_vm_unmark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
#ifdef JEMALLOC_HAVE_MPROTECT
	bool   head_and_tail = (head != NULL) && (tail != NULL);
	size_t range = head_and_tail ? (uintptr_t)tail - (uintptr_t)head + PAGE
	                             : SIZE_T_MAX;
	/*
	 * The amount of work that the kernel does in mprotect depends on the
	 * range argument.  SC_LARGE_MINCLASS is an arbitrary threshold chosen
	 * to prevent kernel from doing too much work that would outweigh the
	 * savings of performing one less system call.
	 */
	bool ranged_mprotect = head_and_tail && range <= SC_LARGE_MINCLASS;
	if (ranged_mprotect) {
		mprotect(head, range, PROT_READ | PROT_WRITE);
	} else {
		if (head != NULL) {
			mprotect(head, PAGE, PROT_READ | PROT_WRITE);
		}
		if (tail != NULL) {
			mprotect(tail, PAGE, PROT_READ | PROT_WRITE);
		}
	}
#else
	if (head != NULL) {
		os_vm_commit(head, PAGE);
	}
	if (tail != NULL) {
		os_vm_commit(tail, PAGE);
	}
#endif
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_purge_lazy(void *addr, size_t size) {
#if defined(JEMALLOC_PURGE_MADVISE_FREE)
	return (madvise(addr, size,
#  ifdef MADV_FREE
	            MADV_FREE
#  else
	            JEMALLOC_MADV_FREE
#  endif
	            )
	    != 0);
#elif defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                \
    && !defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED)                          \
    && !defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#else
	(void)addr;
	(void)size;
	not_reached();
#endif
}

JEMALLOC_ALWAYS_INLINE size_t
os_vm_page_size(void) {
#ifdef __FreeBSD__
	/*
	 * This returns the value obtained from
	 * the auxv vector, avoiding a syscall.
	 */
	return getpagesize();
#else
	long result = sysconf(_SC_PAGESIZE);
	if (result == -1) {
		return PAGE;
	}
	return (size_t)result;
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_VM_H */
