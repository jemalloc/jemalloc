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

/*
 * Shared by every POSIX platform: the four predicates below reproduce, in one
 * place, exactly which platforms actually get pthread_atfork() registered.
 *   - JEMALLOC_HAVE_PTHREAD_ATFORK: the function exists.
 *   - !JEMALLOC_MUTEX_INIT_CB: on FreeBSD-libthr, _pthread_mutex_init_calloc_cb
 *     makes libthr's own _malloc_prefork/_malloc_postfork drive the fork
 *     dance instead.  Registering here would be redundant.
 *   - !JEMALLOC_ZONE: on Darwin, malloc-zone fork callbacks subsume
 *     pthread_atfork.
 *   - !__native_client__: Native Client's sandbox doesn't support fork() at
 *     all, and pthread_atfork() risked being an unresolved symbol in some
 *     NaCl toolchains.  Registering it is both pointless and unsafe there.
 */
JEMALLOC_ALWAYS_INLINE bool
os_process_register_atfork(void (*prepare)(void), void (*parent)(void),
    void (*child)(void)) {
#if defined(JEMALLOC_HAVE_PTHREAD_ATFORK) && !defined(JEMALLOC_MUTEX_INIT_CB) \
    && !defined(JEMALLOC_ZONE) && !defined(__native_client__)
	return pthread_atfork(prepare, parent, child) != 0;
#else
	(void)prepare;
	(void)parent;
	(void)child;
	return false;
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_PROCESS_H */
