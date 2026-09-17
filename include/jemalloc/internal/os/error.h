#ifndef JEMALLOC_INTERNAL_OS_ERROR_H
#define JEMALLOC_INTERNAL_OS_ERROR_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Error-code interface: get/set the calling thread's last-error value, and
 * render an error code to a human-readable string.
 * Default: posix/.  Override: Windows.
 *
 * Included directly by util.h (not via the os.h umbrella): util.h is itself
 * transitively included very early.  assert.h includes malloc_io.h then
 * util.h, before either has any reason to know about mutex.h/base.h.  Thus,
 * pulling in the full os.h from util.h would risk the same circular-include
 * failure worked around in the VM module (base.h -> mutex.h -> os.h ->
 * os/vm.h -> base.h).  This module's backends need nothing beyond libc/CRT
 * error primitives and malloc_io.h's malloc_snprintf, so including it alone
 * from util.h is safe.
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE int os_errno_get(void);
JEMALLOC_ALWAYS_INLINE void os_errno_set(int errnum);
JEMALLOC_ALWAYS_INLINE int os_strerror(int err, char *buf, size_t buflen);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/error.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/error.h"
#else
#  error "OS layer: no error backend for this platform; add os/<os>/error.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_ERROR_H */
