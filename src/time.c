#include "jemalloc/internal/jemalloc_internal.h"

#define	BILLION		1000000000

UNUSED static bool
time_valid(const struct timespec *time)
{

	if (time->tv_sec > TIME_SEC_MAX)
		return (false);
	if (time->tv_nsec >= BILLION)
		return (false);

	return (true);
}

void
time_init(struct timespec *time, time_t sec, long nsec)
{

	time->tv_sec = sec;
	time->tv_nsec = nsec;

	assert(time_valid(time));
}

time_t
time_sec(const struct timespec *time)
{

	assert(time_valid(time));

	return (time->tv_sec);
}

long
time_nsec(const struct timespec *time)
{

	assert(time_valid(time));

	return (time->tv_nsec);
}

void
time_copy(struct timespec *time, const struct timespec *source)
{

	assert(time_valid(source));

	*time = *source;
}

int
time_compare(const struct timespec *a, const struct timespec *b)
{
	int ret;

	assert(time_valid(a));
	assert(time_valid(b));

	ret = (a->tv_sec > b->tv_sec) - (a->tv_sec < b->tv_sec);
	if (ret == 0)
		ret = (a->tv_nsec > b->tv_nsec) - (a->tv_nsec < b->tv_nsec);

	return (ret);
}

void
time_add(struct timespec *time, const struct timespec *addend)
{

	assert(time_valid(time));
	assert(time_valid(addend));

	time->tv_sec += addend->tv_sec;
	time->tv_nsec += addend->tv_nsec;
	if (time->tv_nsec >= BILLION) {
		time->tv_sec++;
		time->tv_nsec -= BILLION;
	}

	assert(time_valid(time));
}

void
time_subtract(struct timespec *time, const struct timespec *subtrahend)
{

	assert(time_valid(time));
	assert(time_valid(subtrahend));
	assert(time_compare(time, subtrahend) >= 0);

	time->tv_sec -= subtrahend->tv_sec;
	if (time->tv_nsec < subtrahend->tv_nsec) {
		time->tv_sec--;
		time->tv_nsec += BILLION;
	}
	time->tv_nsec -= subtrahend->tv_nsec;
}

void
time_imultiply(struct timespec *time, uint64_t multiplier)
{
	time_t sec;
	uint64_t nsec;

	assert(time_valid(time));

	sec = time->tv_sec * multiplier;
	nsec = time->tv_nsec * multiplier;
	sec += nsec / BILLION;
	nsec %= BILLION;
	time_init(time, sec, (long)nsec);

	assert(time_valid(time));
}

void
time_idivide(struct timespec *time, uint64_t divisor)
{
	time_t sec;
	uint64_t nsec;

	assert(time_valid(time));

	sec = time->tv_sec / divisor;
	nsec = ((time->tv_sec % divisor) * BILLION + time->tv_nsec) / divisor;
	sec += nsec / BILLION;
	nsec %= BILLION;
	time_init(time, sec, (long)nsec);

	assert(time_valid(time));
}

uint64_t
time_divide(const struct timespec *time, const struct timespec *divisor)
{
	uint64_t t, d;

	assert(time_valid(time));
	assert(time_valid(divisor));

	t = time_sec(time) * BILLION + time_nsec(time);
	d = time_sec(divisor) * BILLION + time_nsec(divisor);
	assert(d != 0);
	return (t / d);
}

bool
time_update(struct timespec *time)
{
	struct timespec old_time;

	assert(time_valid(time));

	time_copy(&old_time, time);

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
	if (unlikely(time_compare(&old_time, time) > 0)) {
		time_copy(time, &old_time);
		return (true);
	}

	assert(time_valid(time));
	return (false);
}
