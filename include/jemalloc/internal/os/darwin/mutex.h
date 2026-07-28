#ifndef JEMALLOC_INTERNAL_OS_DARWIN_MUTEX_H
#define JEMALLOC_INTERNAL_OS_DARWIN_MUTEX_H

/*
 * Darwin os_unfair_lock backend (macOS 10.12+, selected via
 * JEMALLOC_OS_UNFAIR_LOCK at configure time). os_unfair_lock has no destroy
 * and no postfork_child fixup beyond resetting to OS_UNFAIR_LOCK_INIT (it's
 * a single atomic word).  Older macOS without os_unfair_lock falls back to
 * os/posix/mutex.h's pthread backend instead of this file (see the
 * dispatcher in os/mutex.h).
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#include <os/lock.h>

typedef os_unfair_lock os_mutex_t;
#define OS_MUTEX_INITIALIZER OS_UNFAIR_LOCK_INIT
#define OS_MUTEX_HAS_STATIC_INIT 1

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	os_unfair_lock_lock(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	os_unfair_lock_unlock(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !os_unfair_lock_trylock(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	*m = (os_unfair_lock)OS_UNFAIR_LOCK_INIT;
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_DARWIN_MUTEX_H */
