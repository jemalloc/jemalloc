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

/* Windows has no fork(2)/pthread_atfork; always a no-op. */
JEMALLOC_ALWAYS_INLINE bool
os_process_register_atfork(void (*prepare)(void), void (*parent)(void),
    void (*child)(void)) {
	(void)prepare;
	(void)parent;
	(void)child;
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_PROCESS_H */
