#ifndef JEMALLOC_INTERNAL_OS_H
#define JEMALLOC_INTERNAL_OS_H

/*
 * OS layer.
 *
 * Portable code includes this header to reach the OS-touching primitives it
 * needs.  Each facility is a module with its own dispatcher (os/<module>.h)
 * that selects an implementation:
 *
 *   os/posix/<module>.h  - the default, used by every POSIX platform.
 *   os/<os>/<module>.h   - an override, present ONLY when an OS specializes
 *                          that module (e.g. windows/file.h).
 *
 * A dispatcher picks the OS-specific file when one exists and otherwise falls
 * back to posix/ (guarded by JEMALLOC_OS_POSIX), so any POSIX platform builds
 * without being enumerated anywhere.  A non-POSIX platform with no override
 * hits a #error.  Each os/<module>.h also declares the function prototypes
 * its backends must implement, so a backend with a missing or mismatched
 * function fails to compile instead of silently diverging.
 *
 * Adding OS support for a module (only when existing module headers cannot be
 * reused): create os/<os>/<module>.h (and later a matching src body when
 * necessary and add one branch to os/<module>.h.  Adding a whole new module:
 * module: create os/<module>.h + os/posix/<module>.h and #include it below.
 */

/* Process */
#include "jemalloc/internal/os/process.h"

/* File I/O */
#include "jemalloc/internal/os/file.h"

#endif /* JEMALLOC_INTERNAL_OS_H */
