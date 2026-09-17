#ifndef JEMALLOC_INTERNAL_OS_POSIX_SIGMASK_H
#define JEMALLOC_INTERNAL_OS_POSIX_SIGMASK_H

/*
 * POSIX sigset_t-based signal-mask backend.  Solely backs the background
 * thread's pthread_create wrapper.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

typedef sigset_t os_sigmask_t;

/* Mask all signals, returning the prior mask in *saved. */
JEMALLOC_ALWAYS_INLINE int
os_sigmask_all_enter(os_sigmask_t *saved) {
	sigset_t set;
	sigfillset(&set);
	return pthread_sigmask(SIG_SETMASK, &set, saved);
}

JEMALLOC_ALWAYS_INLINE int
os_sigmask_leave(const os_sigmask_t *saved) {
	return pthread_sigmask(SIG_SETMASK, saved, NULL);
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_SIGMASK_H */
