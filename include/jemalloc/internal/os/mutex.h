#ifndef JEMALLOC_INTERNAL_OS_MUTEX_H
#define JEMALLOC_INTERNAL_OS_MUTEX_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Sync (mutex) interface.
 * Default: posix/.  Override: Darwin (os_unfair_lock), Windows (SRWLOCK /
 * CRITICAL_SECTION).
 *
 * Unlike the other os/<module>.h dispatchers, the enforced function
 * prototypes come AFTER the backend #include below, not before: os_mutex_t
 * is a type (embedded by value in malloc_mutex_t), not just a function
 * parameter, so it must be fully defined by the backend before it can
 * appear in a prototype at all.
 *
 * Capability macro: OS_MUTEX_HAS_STATIC_INIT: 1 iff OS_MUTEX_INITIALIZER is
 * a valid static initializer for os_mutex_t, 0 if the mutex can only be
 * initialized dynamically (via os_mutex_init()).  Defined by whichever
 * backend is selected below.
 */
#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/mutex.h"
#elif defined(JEMALLOC_OS_UNFAIR_LOCK)
#  include "jemalloc/internal/os/darwin/mutex.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/mutex.h"
#else
#  error "OS layer: no mutex backend for this platform; add os/<os>/mutex.h"
#endif

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE void os_mutex_lock(os_mutex_t *m);
JEMALLOC_ALWAYS_INLINE void os_mutex_unlock(os_mutex_t *m);
JEMALLOC_ALWAYS_INLINE bool os_mutex_trylock(os_mutex_t *m);
JEMALLOC_ALWAYS_INLINE bool os_mutex_init(os_mutex_t *m);

#endif /* JEMALLOC_INTERNAL_OS_MUTEX_H */
