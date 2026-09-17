#ifndef JEMALLOC_INTERNAL_OS_DETECT_H
#define JEMALLOC_INTERNAL_OS_DETECT_H

/*
 * Platform detection for the OS-layer dispatchers.
 *
 * Defines JEMALLOC_OS_POSIX when the target is POSIX, so a module dispatcher
 * can fall back to os/posix/<module>.h for ANY POSIX platform without that
 * platform being enumerated.  We treat the target as POSIX if the C library
 * advertises _POSIX_VERSION (via <unistd.h>) or the compiler predefines
 * __unix__/__unix.  Windows is handled by its own _WIN32 branch and never
 * reaches here.
 *
 * Included (idempotently) by os.h and by every module dispatcher, so the
 * dispatchers work whether reached through os.h or directly.
 */
#if !defined(_WIN32)
#  if !defined(__has_include) || __has_include(<unistd.h>)
#    include <unistd.h>
#  endif
#  if defined(_POSIX_VERSION) || defined(__unix__) || defined(__unix)
#    define JEMALLOC_OS_POSIX
#  endif
#endif

#endif /* JEMALLOC_INTERNAL_OS_DETECT_H */
