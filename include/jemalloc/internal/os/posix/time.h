#ifndef JEMALLOC_INTERNAL_OS_POSIX_TIME_H
#define JEMALLOC_INTERNAL_OS_POSIX_TIME_H

/*
 * POSIX time backend: picks the best available (ideally monotonic) clock,
 * falling back to gettimeofday() if nothing better is available.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#if defined(JEMALLOC_HAVE_CLOCK_MONOTONIC_COARSE)
#  define OS_TIME_MONOTONIC 1
JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
}
#elif defined(JEMALLOC_HAVE_CLOCK_MONOTONIC)
#  define OS_TIME_MONOTONIC 1
JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
}
#elif defined(JEMALLOC_HAVE_CLOCK_GETTIME_NSEC_NP)
#  define OS_TIME_MONOTONIC 1
JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	nstime_init(time, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}
#elif defined(JEMALLOC_HAVE_MACH_ABSOLUTE_TIME)
#  define OS_TIME_MONOTONIC 1
JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	static mach_timebase_info_data_t sTimebaseInfo;
	if (sTimebaseInfo.denom == 0) {
		(void)mach_timebase_info(&sTimebaseInfo);
	}
	nstime_init(time,
	    mach_absolute_time() * sTimebaseInfo.numer / sTimebaseInfo.denom);
}
#else
#  define OS_TIME_MONOTONIC 0
JEMALLOC_ALWAYS_INLINE void
os_time_get(nstime_t *time) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	nstime_init2(time, tv.tv_sec, tv.tv_usec * 1000);
}
#endif

JEMALLOC_ALWAYS_INLINE void
os_time_get_realtime(nstime_t *time) {
#if defined(JEMALLOC_HAVE_CLOCK_REALTIME)
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
#else
	unreachable();
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_TIME_H */
