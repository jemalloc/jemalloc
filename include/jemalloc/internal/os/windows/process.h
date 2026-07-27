#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_PROCESS_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_PROCESS_H

#include "jemalloc/internal/jemalloc_preamble.h"

/*
 * Windows process backend (GetCurrentProcessId()).
 */

JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)GetCurrentProcessId();
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_PROCESS_H */
