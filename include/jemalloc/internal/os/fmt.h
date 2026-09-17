#ifndef JEMALLOC_INTERNAL_OS_FMT_H
#define JEMALLOC_INTERNAL_OS_FMT_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Printf format-specifier macros for fixed-width and pointer-sized integers
 * (FMTd32/FMTu32/FMTx32/FMTd64/FMTu64/FMTx64/FMTdPTR/FMTuPTR/FMTxPTR).
 * Default: posix/ (<inttypes.h>'s PRId32 etc).  Override: Windows (MSVC's
 * CRT historically didn't support the C99 <inttypes.h> macros the same way,
 * so these are hand-rolled from an ll/I64-style prefix instead).
 *
 * Unlike other modules, this one defines only macros, no functions.
 */

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/fmt.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/fmt.h"
#else
#  error "OS layer: no fmt backend for this platform; add os/<os>/fmt.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_FMT_H */
