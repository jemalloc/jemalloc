#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_MUTEX_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_MUTEX_H

/*
 * Windows mutex backend.  On Vista+, os_mutex_t is SRWLOCK (lighter than
 * CRITICAL_SECTION); older targets fall back to CRITICAL_SECTION with a
 * spin count.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#if _WIN32_WINNT >= 0x0600
typedef SRWLOCK os_mutex_t;
#  define OS_MUTEX_INITIALIZER SRWLOCK_INIT
#  define OS_MUTEX_HAS_STATIC_INIT 1

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	AcquireSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	ReleaseSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !TryAcquireSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	InitializeSRWLock(m);
	return false;
}
#else
typedef CRITICAL_SECTION os_mutex_t;
/*
 * CRITICAL_SECTION has no static initializer, unlike SRWLOCK above -- so
 * MALLOC_MUTEX_INITIALIZER is empty here, and jemalloc_init.c's init_lock
 * (the one static malloc_mutex_t that must be usable before any other code
 * runs, on every _WIN32_WINNT target) still needs its own constructor-based
 * lazy-init workaround on this pre-Vista path.
 */
#  define OS_MUTEX_HAS_STATIC_INIT 0

#  ifndef _CRT_SPINCOUNT
#    define _CRT_SPINCOUNT 4000
#  endif

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	EnterCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	LeaveCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !TryEnterCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	return !InitializeCriticalSectionAndSpinCount(m, _CRT_SPINCOUNT);
}
#endif

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_MUTEX_H */
