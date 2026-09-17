#ifndef JEMALLOC_INTERNAL_OS_POSIX_COND_H
#define JEMALLOC_INTERNAL_OS_POSIX_COND_H

/*
 * POSIX pthread condvar backend.  Solely backs the background thread.
 *
 * No os_cond_destroy: the background thread is the only cond user and never
 * destroys its condvars (it never did, even pre-refactor), so it would be
 * dead code.  Add one here if a caller ever needs it.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

typedef pthread_cond_t os_cond_t;

JEMALLOC_ALWAYS_INLINE bool
os_cond_init(os_cond_t *c) {
#ifdef JEMALLOC_HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC
	/*
	 * Pair the condvar with CLOCK_MONOTONIC so os_cond_timedwait deadlines
	 * built from os_cond_now() are immune to wall-clock jumps.
	 */
	pthread_condattr_t attr;
	if (pthread_condattr_init(&attr) != 0) {
		return true;
	}
	if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
		pthread_condattr_destroy(&attr);
		return true;
	}
	bool err = (pthread_cond_init(c, &attr) != 0);
	pthread_condattr_destroy(&attr);
	return err;
#else
	return pthread_cond_init(c, NULL) != 0;
#endif
}

JEMALLOC_ALWAYS_INLINE int
os_cond_wait(os_cond_t *c, os_mutex_t *m) {
	return pthread_cond_wait(c, m);
}

/*
 * abs_ts is an absolute deadline in os_cond_now()'s clock (the clock the
 * condvar was paired with in os_cond_init).  Returns 0 on signal, ETIMEDOUT
 * on timeout, other errno otherwise.
 */
JEMALLOC_ALWAYS_INLINE int
os_cond_timedwait(os_cond_t *c, os_mutex_t *m, const struct timespec *abs_ts) {
	return pthread_cond_timedwait(c, m, abs_ts);
}

JEMALLOC_ALWAYS_INLINE void
os_cond_signal(os_cond_t *c) {
	pthread_cond_signal(c);
}

/*
 * Read "now" in the same clock os_cond_init() paired the condvar with, for
 * building os_cond_timedwait() deadlines: CLOCK_MONOTONIC when available,
 * else the wall clock.
 */
JEMALLOC_ALWAYS_INLINE void
os_cond_now(struct timespec *now) {
#ifdef JEMALLOC_HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC
	clock_gettime(CLOCK_MONOTONIC, now);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	now->tv_sec = tv.tv_sec;
	now->tv_nsec = tv.tv_usec * 1000;
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_COND_H */
