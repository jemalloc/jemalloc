#ifndef JEMALLOC_DETERMINISTIC_H
#define JEMALLOC_DETERMINISTIC_H

#if defined(JEMALLOC_DETERMINISTIC_SCHED)

struct waitlist_s;
typedef struct waitlist_s waitlist_t;

/* Public API for tests */
JEMALLOC_EXPORT JEMALLOC_NOTHROW void
det_schedule(unsigned seed, unsigned count,
             void *(*proc)(void *), void *arg);

/* Instrumentation needed internally in jemalloc lib */
void det_before_shared();
void det_after_shared();
bool det_active();
void det_wait(waitlist_t** waiters);
void det_wake(waitlist_t** waiters);

#else /* JEMALLOC_DETERMINISTIC_SCHED */

JEMALLOC_ALWAYS_INLINE void det_before_shared() {}
JEMALLOC_ALWAYS_INLINE void det_after_shared() {}
JEMALLOC_ALWAYS_INLINE void det_active() {}
JEMALLOC_ALWAYS_INLINE void det_wake() {}

#endif /* JEMALLOC_DETERMINISTIC_SCHED */

#endif /* JEMALLOC_DETERMINISTIC_H */
