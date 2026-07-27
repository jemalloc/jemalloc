#ifndef JEMALLOC_INTERNAL_OS_POSIX_PROCESS_H
#define JEMALLOC_INTERNAL_OS_POSIX_PROCESS_H

/*
 * POSIX process backend (getpid()).
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#include <sys/types.h>
#include <unistd.h>

JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)getpid();
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_PROCESS_H */
