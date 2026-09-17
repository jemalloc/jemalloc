#ifndef JEMALLOC_INTERNAL_OS_POSIX_MUTEX_H
#define JEMALLOC_INTERNAL_OS_POSIX_MUTEX_H

/*
 * POSIX pthread mutex backend.  Default for every POSIX platform, including
 * Darwin builds without os_unfair_lock (older macOS).
 */
#include "jemalloc/internal/jemalloc_preamble.h"

typedef pthread_mutex_t os_mutex_t;
#define OS_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define OS_MUTEX_HAS_STATIC_INIT 1

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	pthread_mutex_lock(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	pthread_mutex_unlock(m);
}

/* Returns true on failure (matches MALLOC_MUTEX_TRYLOCK semantics). */
JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return pthread_mutex_trylock(m) != 0;
}

/* Returns true on failure. */
JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		return true;
	}
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
	if (pthread_mutex_init(m, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return true;
	}
	pthread_mutexattr_destroy(&attr);
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_MUTEX_H */
