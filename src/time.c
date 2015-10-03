#include "jemalloc/internal/jemalloc_internal.h"

#ifndef _WIN32
#include <time.h>
#endif

/******************************************************************************/

uint64_t
malloc_time_get_ns(void)
{
#ifndef _WIN32
	struct timespec now;
#endif
	uint64_t ret = 0;

#ifndef _WIN32
#ifdef CLOCK_MONOTONIC_COARSE
	if (likely(clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == 0))
#else
	if (likely(clock_gettime(CLOCK_MONOTONIC, &now) == 0))
#endif
		ret = now.tv_sec * NANOSECONDS_PER_SECOND + now.tv_nsec;
#endif

	return (ret);
}
