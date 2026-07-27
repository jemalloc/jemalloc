#ifndef JEMALLOC_INTERNAL_OS_FILE_H
#define JEMALLOC_INTERNAL_OS_FILE_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * File I/O interface.
 * Default: posix/.  Override: Windows (CRT <io.h>).
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE ssize_t os_file_write_once(int fd, const void *buf,
    size_t bytes);
JEMALLOC_ALWAYS_INLINE ssize_t os_file_read_once(int fd, void *buf,
    size_t bytes);
/*
 * Full retry-until-bytes-or-error versions of the above: loop over the
 * _once primitive until target bytes are transferred or a non-retryable
 * error occurs. Retrying on EINTR is a POSIX-only concept (Windows has no
 * such signal-interruption semantics for this call), so that decision lives
 * entirely in each backend rather than leaking into callers.
 */
JEMALLOC_ALWAYS_INLINE ssize_t os_file_write(int fd, const void *buf,
    size_t bytes);
JEMALLOC_ALWAYS_INLINE ssize_t os_file_read(int fd, void *buf, size_t bytes);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/file.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/file.h"
#else
#  error "OS layer: no file backend for this platform; add os/<os>/file.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_FILE_H */
