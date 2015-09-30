#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

uint64_t
malloc_time_get(void)
{
#ifndef _WIN32
	struct timespec now;
#endif
	int ret;

#ifndef _WIN32
	ret = clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
	if (likely(ret == 0))
		return (now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec);
#endif

	return (0);
}
