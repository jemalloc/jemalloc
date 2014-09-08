/*
 * Simple timer, for use in benchmark reporting.
 */

#include <sys/time.h>

typedef struct {
	struct timeval tv0;
	struct timeval tv1;
} timedelta_t;

void	timer_start(timedelta_t *timer);
void	timer_stop(timedelta_t *timer);
uint64_t	timer_usec(const timedelta_t *timer);
void	timer_ratio(timedelta_t *a, timedelta_t *b, char *buf, size_t buflen);
