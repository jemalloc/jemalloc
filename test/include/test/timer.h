/*
 * Simple timer, for use in benchmark reporting.
 */

#include <sys/time.h>

typedef struct {
	struct timeval tv0;
	struct timeval tv1;
} timer_t;

void	timer_start(timer_t *timer);
void	timer_stop(timer_t *timer);
uint64_t	timer_usec(const timer_t *timer);
void	timer_ratio(timer_t *a, timer_t *b, char *buf, size_t buflen);
