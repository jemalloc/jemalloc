#include "test/jemalloc_test.h"

TEST_BEGIN(test_next_event_fast) {
	tsd_t *tsd = tsd_fetch();
	te_ctx_t ctx;
	te_ctx_get(tsd, &ctx, true);

	te_ctx_last_event_set(&ctx, 0);
	te_ctx_current_bytes_set(&ctx, TE_NEXT_EVENT_FAST_MAX - 8U);
	te_ctx_next_event_set(tsd, &ctx, TE_NEXT_EVENT_FAST_MAX);

	uint64_t *waits = tsd_te_datap_get_unsafe(tsd)->alloc_wait;
	for (size_t i = 0; i < te_alloc_count; i++) {
		waits[i] = TE_NEXT_EVENT_FAST_MAX;
	}

	/* Test next_event_fast rolling back to 0. */
	void *p = malloc(16U);
	assert_ptr_not_null(p, "malloc() failed");
	free(p);

	/* Test next_event_fast resuming to be equal to next_event. */
	void *q = malloc(SC_LOOKUP_MAXCLASS);
	assert_ptr_not_null(q, "malloc() failed");
	free(q);
}
TEST_END

int
main(void) {
	return test(
	    test_next_event_fast);
}
