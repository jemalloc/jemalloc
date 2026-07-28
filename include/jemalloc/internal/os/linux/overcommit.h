#ifndef JEMALLOC_INTERNAL_OS_LINUX_OVERCOMMIT_H
#define JEMALLOC_INTERNAL_OS_LINUX_OVERCOMMIT_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

#include <sys/mman.h>

#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
/*
 * Set by os_overcommit_boot()'s madvise_MADV_DONTNEED_zeroes_pages() probe,
 * consumed by pages_purge_forced() in src/pages.c. Kept behind this macro
 * (rather than assumed unconditional, even though every configure.ac Linux
 * branch defines it today) so this file doesn't silently desync from
 * configure.ac if that ever changes.
 *
 * Defined in src/pages.c.
 */
extern int madvise_dont_need_zeros_is_faulty;
#endif

/* Defined in src/pages.c. */
extern int mmap_flags;

#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
/*
 * Check that MADV_DONTNEED will actually zero pages on subsequent access.
 *
 * Since qemu does not support this, yet [1], and you can get very tricky
 * assert if you will run program with jemalloc in use under qemu:
 *
 *     <jemalloc>: ../contrib/jemalloc/src/extent.c:1195: Failed assertion: "p[i] == 0"
 *
 *   [1]: https://patchwork.kernel.org/patch/10576637/
 */
JEMALLOC_ALWAYS_INLINE int
madvise_MADV_DONTNEED_zeroes_pages(void) {
	size_t size = PAGE;

	void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (addr == MAP_FAILED) {
		malloc_write(
		    "<jemalloc>: Cannot allocate memory for "
		    "MADV_DONTNEED check\n");
		if (opt_abort) {
			abort();
		}
	}

	memset(addr, 'A', size);
	int works;
	if (madvise(addr, size, MADV_DONTNEED) == 0) {
		works = memchr(addr, 'A', size) == NULL;
	} else {
		/*
		 * If madvise() does not support MADV_DONTNEED, then we can
		 * call it anyway, and use it's return code.
		 */
		works = 1;
	}

	if (munmap(addr, size) != 0) {
		malloc_write(
		    "<jemalloc>: Cannot deallocate memory for "
		    "MADV_DONTNEED check\n");
		if (opt_abort) {
			abort();
		}
	}

	return works;
}
#endif

JEMALLOC_ALWAYS_INLINE bool
os_overcommits_proc(void) {
	int  fd;
	char buf[1];

#  if defined(O_CLOEXEC)
	fd = malloc_open(
	    "/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
#  else
	fd = malloc_open("/proc/sys/vm/overcommit_memory", O_RDONLY);
	if (fd != -1) {
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
#  endif

	if (fd == -1) {
		return false; /* Error. */
	}

	ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
	malloc_close(fd);

	if (nread < 1) {
		return false; /* Error. */
	}
	/*
	 * /proc/sys/vm/overcommit_memory meanings:
	 * 0: Heuristic overcommit.
	 * 1: Always overcommit.
	 * 2: Never overcommit.
	 */
	return (buf[0] == '0' || buf[0] == '1');
}

/*
 * Bundles what pages_boot() used to do after the page-size check, on Linux:
 *   1. DONTNEED-zeros probe (madvise_MADV_DONTNEED_zeroes_pages()).
 *   2. mmap_flags assembly (MAP_PRIVATE | MAP_ANON, plus MAP_NORESERVE when
 *      the kernel overcommits).
 *   3. Overcommit detection via /proc/sys/vm/overcommit_memory
 *      (os_overcommits_proc() above).
 */
JEMALLOC_ALWAYS_INLINE bool
os_overcommit_boot(void) {
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
	if (!opt_trust_madvise) {
		madvise_dont_need_zeros_is_faulty =
		    !madvise_MADV_DONTNEED_zeroes_pages();
		if (madvise_dont_need_zeros_is_faulty) {
			malloc_write(
			    "<jemalloc>: MADV_DONTNEED does not work (memset will be used instead)\n");
			malloc_write(
			    "<jemalloc>: (This is the expected behaviour if you are running under QEMU)\n");
		}
	} else {
		/*
		 * In case opt_trust_madvise is disable,
		 * do not do runtime check.
		 */
		madvise_dont_need_zeros_is_faulty = 0;
	}
#endif

	mmap_flags = MAP_PRIVATE | MAP_ANON;

	os_overcommits = os_overcommits_proc();
#ifdef MAP_NORESERVE
	if (os_overcommits) {
		mmap_flags |= MAP_NORESERVE;
	}
#endif

	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_LINUX_OVERCOMMIT_H */
