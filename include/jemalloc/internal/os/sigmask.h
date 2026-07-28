#ifndef JEMALLOC_INTERNAL_OS_SIGMASK_H
#define JEMALLOC_INTERNAL_OS_SIGMASK_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Signal-mask interface.  Exists only when background threads are supported
 * (JEMALLOC_BACKGROUND_THREAD), for the same reason as os/cond.h: it backs
 * the background thread (masking signals during pthread_create so the new
 * thread inherits an empty signal set), which is pthread-specific and never
 * built on Windows -- and never enabled on Darwin either, despite Darwin
 * being POSIX.  Gating on JEMALLOC_BACKGROUND_THREAD first, rather than
 * JEMALLOC_OS_POSIX alone, means this header is a no-op wherever background
 * threads aren't needed, so os.h can include it unconditionally.
 */
#if defined(JEMALLOC_BACKGROUND_THREAD)
#  if defined(JEMALLOC_OS_POSIX)
#    include "jemalloc/internal/os/posix/sigmask.h"
#  else
#    error "OS layer: no sigmask backend for this platform; add os/<os>/sigmask.h"
#  endif

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE int os_sigmask_all_enter(os_sigmask_t *saved);
JEMALLOC_ALWAYS_INLINE int os_sigmask_leave(const os_sigmask_t *saved);
#endif /* defined(JEMALLOC_BACKGROUND_THREAD) */

#endif /* JEMALLOC_INTERNAL_OS_SIGMASK_H */
