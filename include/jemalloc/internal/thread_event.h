#ifndef JEMALLOC_INTERNAL_THREAD_EVENT_H
#define JEMALLOC_INTERNAL_THREAD_EVENT_H

#include "jemalloc/internal/tsd.h"

/*
 * Maximum threshold on thread_(de)allocated_next_event_fast, so that there is
 * no need to check overflow in malloc fast path. (The allocation size in malloc
 * fast path never exceeds SC_LOOKUP_MAXCLASS.)
 */
#define THREAD_NEXT_EVENT_FAST_MAX				\
    (UINT64_MAX - SC_LOOKUP_MAXCLASS + 1U)

/*
 * The max interval helps make sure that malloc stays on the fast path in the
 * common case, i.e. thread_allocated < thread_allocated_next_event_fast.  When
 * thread_allocated is within an event's distance to THREAD_NEXT_EVENT_FAST_MAX
 * above, thread_allocated_next_event_fast is wrapped around and we fall back to
 * the medium-fast path. The max interval makes sure that we're not staying on
 * the fallback case for too long, even if there's no active event or if all
 * active events have long wait times.
 */
#define THREAD_EVENT_MAX_INTERVAL ((uint64_t)(4U << 20))

typedef struct event_ctx_s {
	bool is_alloc;
	uint64_t *current;
	uint64_t *last_event;
	uint64_t *next_event;
	uint64_t *next_event_fast;
} event_ctx_t;

void thread_event_assert_invariants_debug(tsd_t *tsd);
void thread_event_trigger(tsd_t *tsd, event_ctx_t *ctx, bool delay_event);
void thread_alloc_event_rollback(tsd_t *tsd, size_t diff);
void thread_event_update(tsd_t *tsd, bool alloc_event);
void thread_event_recompute_fast_threshold(tsd_t *tsd);
void tsd_thread_event_init(tsd_t *tsd);

/*
 * List of all events, in the following format:
 *  E(event,		(condition), is_alloc_event)
 */
#define ITERATE_OVER_ALL_EVENTS						\
    E(tcache_gc,	(TCACHE_GC_INCR_BYTES > 0), true)		\
    E(prof_sample,	(config_prof && opt_prof), true)	    	\
    E(stats_interval,	(opt_stats_interval >= 0), true)	    	\
    E(tcache_gc_dalloc,	(TCACHE_GC_INCR_BYTES > 0), false)

#define E(event, condition_unused, is_alloc_event_unused)		\
    C(event##_event_wait)

/* List of all thread event counters. */
#define ITERATE_OVER_ALL_COUNTERS					\
    C(thread_allocated)							\
    C(thread_allocated_last_event)					\
    ITERATE_OVER_ALL_EVENTS						\
    C(prof_sample_last_event)						\
    C(stats_interval_last_event)

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
 * The malloc and free fastpath getters -- use the unsafe getters since tsd may
 * be non-nominal, in which case the fast_threshold will be set to 0.  This
 * allows checking for events and tsd non-nominal in a single branch.
 *
 * Note that these can only be used on the fastpath.
 */
JEMALLOC_ALWAYS_INLINE uint64_t
thread_allocated_malloc_fastpath(tsd_t *tsd) {
	return *tsd_thread_allocatedp_get_unsafe(tsd);
}

JEMALLOC_ALWAYS_INLINE uint64_t
thread_allocated_next_event_malloc_fastpath(tsd_t *tsd) {
	uint64_t v = *tsd_thread_allocated_next_event_fastp_get_unsafe(tsd);
	assert(v <= THREAD_NEXT_EVENT_FAST_MAX);
	return v;
}

JEMALLOC_ALWAYS_INLINE void
thread_event_free_fastpath_ctx(tsd_t *tsd, uint64_t *deallocated,
    uint64_t *threshold, bool size_hint) {
	if (!size_hint) {
		*deallocated = tsd_thread_deallocated_get(tsd);
		*threshold = tsd_thread_deallocated_next_event_fast_get(tsd);
	} else {
		/* Unsafe getters since this may happen before tsd_init. */
		*deallocated = *tsd_thread_deallocatedp_get_unsafe(tsd);
		*threshold =
		    *tsd_thread_deallocated_next_event_fastp_get_unsafe(tsd);
	}
	assert(*threshold <= THREAD_NEXT_EVENT_FAST_MAX);
}

JEMALLOC_ALWAYS_INLINE bool
event_ctx_is_alloc(event_ctx_t *ctx) {
	return ctx->is_alloc;
}

JEMALLOC_ALWAYS_INLINE uint64_t
event_ctx_current_bytes_get(event_ctx_t *ctx) {
	return *ctx->current;
}

JEMALLOC_ALWAYS_INLINE void
event_ctx_current_bytes_set(event_ctx_t *ctx, uint64_t v) {
	*ctx->current = v;
}

JEMALLOC_ALWAYS_INLINE uint64_t
event_ctx_last_event_get(event_ctx_t *ctx) {
	return *ctx->last_event;
}

JEMALLOC_ALWAYS_INLINE void
event_ctx_last_event_set(event_ctx_t *ctx, uint64_t v) {
	*ctx->last_event = v;
}

/* Below 3 for next_event_fast. */
JEMALLOC_ALWAYS_INLINE uint64_t
event_ctx_next_event_fast_get(event_ctx_t *ctx) {
	uint64_t v = *ctx->next_event_fast;
	assert(v <= THREAD_NEXT_EVENT_FAST_MAX);
	return v;
}

JEMALLOC_ALWAYS_INLINE void
event_ctx_next_event_fast_set(event_ctx_t *ctx, uint64_t v) {
	assert(v <= THREAD_NEXT_EVENT_FAST_MAX);
	*ctx->next_event_fast = v;
}

JEMALLOC_ALWAYS_INLINE void
thread_next_event_fast_set_non_nominal(tsd_t *tsd) {
	/*
	 * Set the fast thresholds to zero when tsd is non-nominal.  Use the
	 * unsafe getter as this may get called during tsd init and clean up.
	 */
	*tsd_thread_allocated_next_event_fastp_get_unsafe(tsd) = 0;
	*tsd_thread_deallocated_next_event_fastp_get_unsafe(tsd) = 0;
}

/* For next_event.  Setter also updates the fast threshold. */
JEMALLOC_ALWAYS_INLINE uint64_t
event_ctx_next_event_get(event_ctx_t *ctx) {
	return *ctx->next_event;
}

JEMALLOC_ALWAYS_INLINE void
event_ctx_next_event_set(tsd_t *tsd, event_ctx_t *ctx, uint64_t v) {
	*ctx->next_event = v;
	thread_event_recompute_fast_threshold(tsd);
}

/*
 * The function checks in debug mode whether the thread event counters are in
 * a consistent state, which forms the invariants before and after each round
 * of thread event handling that we can rely on and need to promise.
 * The invariants are only temporarily violated in the middle of:
 * (a) thread_event() if an event is triggered (the thread_event_trigger() call
 *     at the end will restore the invariants),
 * (b) thread_##event##_event_update() (the thread_event_update() call at the
 *     end will restore the invariants), or
 * (c) thread_alloc_event_rollback() if the rollback falls below the last_event
 *     (the thread_event_update() call at the end will restore the invariants).
 */
JEMALLOC_ALWAYS_INLINE void
thread_event_assert_invariants(tsd_t *tsd) {
	if (config_debug) {
		thread_event_assert_invariants_debug(tsd);
	}
}

JEMALLOC_ALWAYS_INLINE void
event_ctx_get(tsd_t *tsd, event_ctx_t *ctx, bool is_alloc) {
	ctx->is_alloc = is_alloc;
	if (is_alloc) {
		ctx->current = tsd_thread_allocatedp_get(tsd);
		ctx->last_event = tsd_thread_allocated_last_eventp_get(tsd);
		ctx->next_event = tsd_thread_allocated_next_eventp_get(tsd);
		ctx->next_event_fast =
		    tsd_thread_allocated_next_event_fastp_get(tsd);
	} else {
		ctx->current = tsd_thread_deallocatedp_get(tsd);
		ctx->last_event = tsd_thread_deallocated_last_eventp_get(tsd);
		ctx->next_event = tsd_thread_deallocated_next_eventp_get(tsd);
		ctx->next_event_fast =
		    tsd_thread_deallocated_next_event_fastp_get(tsd);
	}
}

JEMALLOC_ALWAYS_INLINE void
thread_event_advance(tsd_t *tsd, size_t usize, bool is_alloc) {
	thread_event_assert_invariants(tsd);

	event_ctx_t ctx;
	event_ctx_get(tsd, &ctx, is_alloc);

	uint64_t bytes_before = event_ctx_current_bytes_get(&ctx);
	event_ctx_current_bytes_set(&ctx, bytes_before + usize);

	/* The subtraction is intentionally susceptible to underflow. */
	if (likely(usize < event_ctx_next_event_get(&ctx) - bytes_before)) {
		thread_event_assert_invariants(tsd);
	} else {
		thread_event_trigger(tsd, &ctx, false);
	}
}

JEMALLOC_ALWAYS_INLINE void
thread_dalloc_event(tsd_t *tsd, size_t usize) {
	thread_event_advance(tsd, usize, false);
}

JEMALLOC_ALWAYS_INLINE void
thread_alloc_event(tsd_t *tsd, size_t usize) {
	thread_event_advance(tsd, usize, true);
}

#define E(event, condition, is_alloc)					\
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
	thread_event_update(tsd, is_alloc);				\
}

ITERATE_OVER_ALL_EVENTS
#undef E

#endif /* JEMALLOC_INTERNAL_THREAD_EVENT_H */
