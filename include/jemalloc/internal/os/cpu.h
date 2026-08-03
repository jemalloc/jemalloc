#ifndef JEMALLOC_INTERNAL_OS_CPU_H
#define JEMALLOC_INTERNAL_OS_CPU_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/os/detect.h"

/*
 * CPU interface: counts and current-CPU queries used for arena / tcache
 * sizing.
 * Default: posix/.  Override: Windows (GetSystemInfo /
 * GetCurrentProcessorNumber), Darwin (os_cpu_current() has no sched_getcpu()
 * to fall back on, so it reads the CPU index directly out of a CPU register
 * -- substantial enough, and different enough per-arch, to warrant its own
 * file rather than an inline #ifdef in posix/cpu.h; os_cpu_ncpus() and
 * os_cpu_count_is_deterministic() are identical to POSIX, so darwin/cpu.h
 * reuses posix/cpu.h for those instead of duplicating them).
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE unsigned os_cpu_ncpus(void);
JEMALLOC_ALWAYS_INLINE bool os_cpu_count_is_deterministic(void);
JEMALLOC_ALWAYS_INLINE int os_cpu_current(void);
/*
 * Pin the calling thread to cpu, returning true on failure. Windows and
 * Darwin never actually run this (background_thread.c, its only caller, is
 * compiled out on both), so their backends are no-ops.
 */
JEMALLOC_ALWAYS_INLINE bool os_cpu_set_affinity(int cpu);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/cpu.h"
#elif defined(__APPLE__)
#  include "jemalloc/internal/os/darwin/cpu.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/cpu.h"
#else
#  error "OS layer: no cpu backend for this platform; add os/<os>/cpu.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_CPU_H */
