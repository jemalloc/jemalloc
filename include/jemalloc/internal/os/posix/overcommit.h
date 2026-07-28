#ifndef JEMALLOC_INTERNAL_OS_POSIX_OVERCOMMIT_H
#define JEMALLOC_INTERNAL_OS_POSIX_OVERCOMMIT_H

#include "jemalloc/internal/jemalloc_preamble.h"

#include <sys/mman.h>

/* Defined in src/pages.c. */
extern int mmap_flags;

JEMALLOC_ALWAYS_INLINE bool
os_overcommit_boot(void) {
	mmap_flags = MAP_PRIVATE | MAP_ANON;
#ifdef __NetBSD__
	os_overcommits = true;
#else
	os_overcommits = false;
#endif
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_OVERCOMMIT_H */
