#ifndef JEMALLOC_INTERNAL_OS_PROCESS_H
#define JEMALLOC_INTERNAL_OS_PROCESS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Process interface.
 * Default: posix/.  Override: Windows (GetCurrentProcessId).
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE int os_process_id(void);
/*
 * Install fork handlers, returning true on failure.  A no-op returning false
 * where fork handler registration doesn't apply.
 */
JEMALLOC_ALWAYS_INLINE bool os_process_register_atfork(
    void (*prepare)(void), void (*parent)(void), void (*child)(void));

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/process.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/process.h"
#else
#  error "OS layer: no process backend for this platform; add os/<os>/process.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_PROCESS_H */
