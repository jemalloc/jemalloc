#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_TIME_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_TIME_H

/*
 * Windows time backend, via GetSystemTimeAsFileTime().  Not monotonic.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#define OS_TIME_MONOTONIC 0

JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	FILETIME ft;
	uint64_t ticks_100ns;

	GetSystemTimeAsFileTime(&ft);
	ticks_100ns = (((uint64_t)ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

	nstime_init(time, ticks_100ns * 100);
}

JEMALLOC_ALWAYS_INLINE void
os_time_get_realtime(nstime_t *time) {
	unreachable();
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_TIME_H */
