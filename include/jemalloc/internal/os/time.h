#ifndef JEMALLOC_INTERNAL_OS_TIME_H
#define JEMALLOC_INTERNAL_OS_TIME_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"
#include "jemalloc/internal/nstime.h"

/*
 * Time interface.
 * Default: posix/.  Override: Windows (GetSystemTimeAsFileTime).
 *
 * Capability macro: OS_TIME_MONOTONIC - 1 iff os_time_get()'s clock never
 * goes backwards, 0 otherwise.  Defined by whichever backend is selected
 * below.
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE void os_time_get(nstime_t *time);
JEMALLOC_ALWAYS_INLINE void os_time_get_realtime(nstime_t *time);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/time.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/time.h"
#else
#  error "OS layer: no time backend for this platform; add os/<os>/time.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_TIME_H */
