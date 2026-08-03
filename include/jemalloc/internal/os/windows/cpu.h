#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_CPU_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_CPU_H

#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE unsigned
os_cpu_ncpus(void) {
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (unsigned)si.dwNumberOfProcessors;
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_count_is_deterministic(void) {
	return true;
}

JEMALLOC_ALWAYS_INLINE int
os_cpu_current(void) {
	return (int)GetCurrentProcessorNumber();
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_set_affinity(int cpu) {
	(void)cpu;
	return false;
}

JEMALLOC_ALWAYS_INLINE void
os_cpu_yield(void) {
	SwitchToThread();
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_CPU_H */
