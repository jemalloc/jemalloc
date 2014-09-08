#include "test/jemalloc_test.h"

void
timer_start(timedelta_t *timer)
{

	gettimeofday(&timer->tv0, NULL);
}

void
timer_stop(timedelta_t *timer)
{

	gettimeofday(&timer->tv1, NULL);
}

uint64_t
timer_usec(const timedelta_t *timer)
{

	return (((timer->tv1.tv_sec - timer->tv0.tv_sec) * 1000000) +
	    timer->tv1.tv_usec - timer->tv0.tv_usec);
}

void
timer_ratio(timedelta_t *a, timedelta_t *b, char *buf, size_t buflen)
{
	uint64_t t0 = timer_usec(a);
	uint64_t t1 = timer_usec(b);
	uint64_t mult;
	unsigned i = 0;
	unsigned j;
	int n;

	/* Whole. */
	n = malloc_snprintf(&buf[i], buflen-i, "%"PRIu64, t0 / t1);
	i += n;
	if (i >= buflen)
		return;
	mult = 1;
	for (j = 0; j < n; j++)
		mult *= 10;

	/* Decimal. */
	n = malloc_snprintf(&buf[i], buflen-i, ".");
	i += n;

	/* Fraction. */
	while (i < buflen-1) {
		uint64_t round = (i+1 == buflen-1 && ((t0 * mult * 10 / t1) % 10
		    >= 5)) ? 1 : 0;
		n = malloc_snprintf(&buf[i], buflen-i,
		    "%"PRIu64, (t0 * mult / t1) % 10 + round);
		i += n;
		mult *= 10;
	}
}
