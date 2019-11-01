#ifndef JEMALLOC_INTERNAL_THREAD_EVENT_H
#define JEMALLOC_INTERNAL_THREAD_EVENT_H

#include "jemalloc/internal/tsd.h"

/*
 * Maximum threshold on thread_allocated_next_event_fast, so that there is no
 * need to check overflow in malloc fast path. (The allocation size in malloc
 * fast path never exceeds SC_LOOKUP_MAXCLASS.)
 */
#define THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX				\
    (UINT64_MAX - SC_LOOKUP_MAXCLASS + 1U)

/*
 * The max interval helps make sure that malloc stays on the fast path in the
 * common case, i.e. thread_allocated < thread_allocated_next_event_fast.
 * When thread_allocated is within an event's distance to
 * THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX above, thread_allocated_next_event_fast
 * is wrapped around and we fall back to the medium-fast path. The max interval
 * makes sure that we're not staying on the fallback case for too long, even if
 * there's no active event or if all active events have long wait times.
 */
#define THREAD_EVENT_MAX_INTERVAL ((uint64_t)(4U << 20))

void thread_event_assert_invariants_debug(tsd_t *tsd);
void thread_event_trigger(tsd_t *tsd, bool delay_event);
void thread_event_rollback(tsd_t *tsd, size_t diff);
void thread_event_update(tsd_t *tsd);
void thread_event_boot();
void tsd_thread_event_init(tsd_t *tsd);

/*
 * List of all events, in the following format:
 *  E(event,		(condition))
 */
#define ITERATE_OVER_ALL_EVENTS						\
    E(tcache_gc,	(TCACHE_GC_INCR_BYTES > 0))			\
    E(prof_sample,	(config_prof && opt_prof))

#define E(event, condition)						\
    C(event##_event_wait)

/* List of all thread event counters. */
#define ITERATE_OVER_ALL_COUNTERS					\
    C(thread_allocated)							\
    C(thread_allocated_next_event_fast)					\
    C(thread_allocated_last_event)					\
    C(thread_allocated_next_event)					\
    ITERATE_OVER_ALL_EVENTS						\
    C(prof_sample_last_event)

/* Getters directly wrap TSD getters. */
#define C(counter)							\
JEMALLOC_ALWAYS_INLINE uint64_t						\
counter##_get(tsd_t *tsd) {						\
	return tsd_##counter##_get(tsd);				\
}

ITERATE_OVER_ALL_COUNTERS
#undef C

/*
 * Setters call the TSD pointer getters rather than the TSD setters, so that
 * the counters can be modified even when TSD state is reincarnated or
 * minimal_initialized: if an event is triggered in such cases, we will
 * temporarily delay the event and let it be immediately triggered at the next
 * allocation call.
 */
#define C(counter)							\
JEMALLOC_ALWAYS_INLINE void						\
counter##_set(tsd_t *tsd, uint64_t v) {					\
	*tsd_##counter##p_get(tsd) = v;					\
}

ITERATE_OVER_ALL_COUNTERS
#undef C

/*
 * For generating _event_wait getter / setter functions for each individual
 * event.
 */
#undef E

/*
 * The function checks in debug mode whether the thread event counters are in
 * a consistent state, which forms the invariants before and after each round
 * of thread event handling that we can rely on and need to promise.
 * The invariants are only temporarily violated in the middle of:
 * (a) thread_event() if an event is triggered (the thread_event_trigger() call
 *     at the end will restore the invariants),
 * (b) thread_##event##_event_update() (the thread_event_update() call at the
 *     end will restore the invariants), or
 * (c) thread_event_rollback() if the rollback falls below the last_event (the
 *     thread_event_update() call at the end will restore the invariants).
 */
JEMALLOC_ALWAYS_INLINE void
thread_event_assert_invariants(tsd_t *tsd) {
	if (config_debug) {
		thread_event_assert_invariants_debug(tsd);
	}
}

JEMALLOC_ALWAYS_INLINE void
thread_event(tsd_t *tsd, size_t usize) {
	thread_event_assert_invariants(tsd);

	uint64_t thread_allocated_before = thread_allocated_get(tsd);
	thread_allocated_set(tsd, thread_allocated_before + usize);

	/* The subtraction is intentionally susceptible to underflow. */
	if (likely(usize < thread_allocated_next_event_get(tsd) -
	    thread_allocated_before)) {
		thread_event_assert_invariants(tsd);
	} else {
		thread_event_trigger(tsd, false);
	}
}

#define E(event, condition)						\
JEMALLOC_ALWAYS_INLINE void						\
thread_##event##_event_update(tsd_t *tsd, uint64_t event_wait) {	\
	thread_event_assert_invariants(tsd);				\
	assert(condition);						\
	assert(tsd_nominal(tsd));					\
	assert(tsd_reentrancy_level_get(tsd) == 0);			\
	assert(event_wait > 0U);					\
	if (THREAD_EVENT_MIN_START_WAIT > 1U &&				\
	    unlikely(event_wait < THREAD_EVENT_MIN_START_WAIT)) {	\
		event_wait = THREAD_EVENT_MIN_START_WAIT;		\
	}								\
	if (THREAD_EVENT_MAX_START_WAIT < UINT64_MAX &&			\
	    unlikely(event_wait > THREAD_EVENT_MAX_START_WAIT)) {	\
		event_wait = THREAD_EVENT_MAX_START_WAIT;		\
	}								\
	event##_event_wait_set(tsd, event_wait);			\
	thread_event_update(tsd);					\
}

ITERATE_OVER_ALL_EVENTS
#undef E

#endif /* JEMALLOC_INTERNAL_THREAD_EVENT_H */
