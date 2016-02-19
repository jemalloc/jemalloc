/* Simple timer, for use in benchmark reporting. */

#include <unistd.h>
#include <sys/time.h>

typedef struct {
	struct timespec t0;
	struct timespec t1;
} timedelta_t;

void	timer_start(timedelta_t *timer);
void	timer_stop(timedelta_t *timer);
uint64_t	timer_usec(const timedelta_t *timer);
void	timer_ratio(timedelta_t *a, timedelta_t *b, char *buf, size_t buflen);
