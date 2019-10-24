#include "test/jemalloc_test.h"

TEST_BEGIN(test_next_event_fast_roll_back) {
	tsd_t *tsd = tsd_fetch();
	thread_allocated_last_event_set(tsd, 0);
	thread_allocated_set(tsd,
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX - 8U);
	thread_allocated_next_event_set(tsd,
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX);
	thread_allocated_next_event_fast_set(tsd,
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX);
#define E(event, condition)						\
	event##_event_wait_set(tsd,					\
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX);
	ITERATE_OVER_ALL_EVENTS
#undef E
	void *p = malloc(16U);
	assert_ptr_not_null(p, "malloc() failed");
	free(p);
}
TEST_END

TEST_BEGIN(test_next_event_fast_resume) {
	tsd_t *tsd = tsd_fetch();
	thread_allocated_last_event_set(tsd, 0);
	thread_allocated_set(tsd,
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX + 8U);
	thread_allocated_next_event_set(tsd,
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX + 16U);
	thread_allocated_next_event_fast_set(tsd, 0);
#define E(event, condition)						\
	event##_event_wait_set(tsd,					\
	    THREAD_ALLOCATED_NEXT_EVENT_FAST_MAX + 16U);
	ITERATE_OVER_ALL_EVENTS
#undef E
	void *p = malloc(SC_LOOKUP_MAXCLASS);
	assert_ptr_not_null(p, "malloc() failed");
	free(p);
}
TEST_END

TEST_BEGIN(test_event_rollback) {
	tsd_t *tsd = tsd_fetch();
	const uint64_t diff = THREAD_EVENT_MAX_INTERVAL >> 2;
	size_t count = 10;
	uint64_t thread_allocated = thread_allocated_get(tsd);
	while (count-- != 0) {
		thread_event_rollback(tsd, diff);
		uint64_t thread_allocated_after = thread_allocated_get(tsd);
		assert_u64_eq(thread_allocated - thread_allocated_after, diff,
		    "thread event counters are not properly rolled back");
		thread_allocated = thread_allocated_after;
	}
}
TEST_END

int
main(void) {
	return test(
	    test_next_event_fast_roll_back,
	    test_next_event_fast_resume,
	    test_event_rollback);
}
