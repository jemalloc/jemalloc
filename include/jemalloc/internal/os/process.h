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

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/process.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/process.h"
#else
#  error "OS layer: no process backend for this platform; add os/<os>/process.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_PROCESS_H */
