#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_OVERCOMMIT_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_OVERCOMMIT_H

#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE bool
os_overcommit_boot(void) {
	/*
	 * Windows never overcommits, so this just hardcodes os_overcommits =
	 * false.  There's no sysctl/proc-file probe to run (contrast with
	 * os_overcommits_sysctl() in os/freebsd/overcommit.h and
	 * os_overcommits_proc() in os/linux/overcommit.h) and no
	 * DONTNEED-zeros probe or mmap_flags to assemble (both
	 * POSIX/madvise-only concepts).
	 */
	os_overcommits = false;
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_OVERCOMMIT_H */
