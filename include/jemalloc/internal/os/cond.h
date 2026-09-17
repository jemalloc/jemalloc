#ifndef JEMALLOC_INTERNAL_OS_COND_H
#define JEMALLOC_INTERNAL_OS_COND_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"
#include "jemalloc/internal/os/mutex.h"

/*
 * Condition-variable interface.  Exists only when background threads are
 * supported (JEMALLOC_BACKGROUND_THREAD, pthread-only, never defined on
 * Windows, and also never defined on Darwin despite Darwin being POSIX, per
 * configure.ac's os_unfair_lock/macho exclusion): this header is a no-op on
 * any platform that doesn't need it, so os.h can include it unconditionally
 * without forcing a backend to exist where none is required.  Only once
 * background threads ARE needed does the inner POSIX-vs-else split apply,
 * enforcing a real backend (os/<os>/cond.h) for any such platform.
 *
 * As with os/mutex.h, the enforced function prototypes come AFTER the
 * backend #include below: os_cond_t is a type (embedded by value in
 * background_thread_info_t), not just a function parameter.
 */
#if defined(JEMALLOC_BACKGROUND_THREAD)
#  if defined(JEMALLOC_OS_POSIX)
#    include "jemalloc/internal/os/posix/cond.h"
#  else
#    error "OS layer: no cond backend for this platform; add os/<os>/cond.h"
#  endif

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE bool os_cond_init(os_cond_t *c);
JEMALLOC_ALWAYS_INLINE int os_cond_wait(os_cond_t *c, os_mutex_t *m);
JEMALLOC_ALWAYS_INLINE int os_cond_timedwait(
    os_cond_t *c, os_mutex_t *m, const struct timespec *abs_ts);
JEMALLOC_ALWAYS_INLINE void os_cond_signal(os_cond_t *c);
JEMALLOC_ALWAYS_INLINE void os_cond_now(struct timespec *now);
#endif /* defined(JEMALLOC_BACKGROUND_THREAD) */

#endif /* JEMALLOC_INTERNAL_OS_COND_H */
