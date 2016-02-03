#include "jemalloc/internal/jemalloc_internal.h"

bool
time_update(struct timespec *time)
{
	struct timespec old_time;

	memcpy(&old_time, time, sizeof(struct timespec));

#ifdef _WIN32
	FILETIME ft;
	uint64_t ticks;
	GetSystemTimeAsFileTime(&ft);
	ticks = (ft.dwHighDateTime << 32) | ft.dWLowDateTime;
	time->tv_sec = ticks / 10000;
	time->tv_nsec = ((ticks % 10000) * 100);
#elif JEMALLOC_CLOCK_GETTIME
	if (sysconf(_SC_MONOTONIC_CLOCK) > 0)
		clock_gettime(CLOCK_MONOTONIC, time);
	else
		clock_gettime(CLOCK_REALTIME, time);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	time->tv_sec = tv.tv_sec;
	time->tv_nsec = tv.tv_usec * 1000;
#endif

	/* Handle non-monotonic clocks. */
	if (unlikely(old_time.tv_sec > time->tv_sec))
		return (true);
	if (unlikely(old_time.tv_sec == time->tv_sec))
		return old_time.tv_nsec > time->tv_nsec;

	return (false);
}
