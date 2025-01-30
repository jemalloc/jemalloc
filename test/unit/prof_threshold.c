#include "test/jemalloc_test.h"

/* Test config (set in reset_test_config) */
#define ALLOC_ITERATIONS_IN_THRESHOLD 10
uint64_t threshold_bytes = 0;
uint64_t chunk_size = 0;

/* Test globals for calblack */
uint64_t hook_calls = 0;
uint64_t last_peak = 0;
uint64_t last_alloc = 0;
uint64_t alloc_baseline = 0;

void
mock_prof_threshold_hook(uint64_t alloc, uint64_t dealloc, uint64_t peak) {
	hook_calls++;
	last_peak = peak;
	last_alloc = alloc;
}

/* Need the do_write flag because NULL is a valid to_write value. */
static void
read_write_prof_threshold_hook(prof_threshold_hook_t *to_read, bool do_write,
    prof_threshold_hook_t to_write) {
	size_t hook_sz = sizeof(prof_threshold_hook_t);
	expect_d_eq(mallctl("experimental.hooks.prof_threshold",
	    (void *)to_read, &hook_sz, do_write ? &to_write : NULL, hook_sz), 0,
	    "Unexpected prof_threshold_hook mallctl failure");
}

static void
write_prof_threshold_hook(prof_threshold_hook_t new_hook) {
	read_write_prof_threshold_hook(NULL, true, new_hook);
}

static prof_threshold_hook_t
read_prof_threshold_hook() {
	prof_threshold_hook_t hook;
	read_write_prof_threshold_hook(&hook, false, NULL);
	return hook;
}

static void reset_test_config() {
	hook_calls = 0;
	last_peak = 0;
	alloc_baseline = last_alloc; /* We run the test multiple times */
	last_alloc = 0;
	threshold_bytes = 1 << opt_experimental_lg_prof_threshold;
	chunk_size = threshold_bytes / ALLOC_ITERATIONS_IN_THRESHOLD;
}

static void expect_threshold_calls(int calls) {
	expect_u64_eq(hook_calls, calls, "Hook called the right amount of times");
	expect_u64_lt(last_peak, chunk_size * 2, "We allocate chunk_size at a time");
	expect_u64_ge(last_alloc, threshold_bytes * calls + alloc_baseline, "Crosses");
}

static void allocate_chunks(int chunks) {
	for (int i = 0; i < chunks; i++) {
		void* p = mallocx((size_t)chunk_size, 0);
		expect_ptr_not_null(p, "Failed to allocate");
		free(p);
	}
}

TEST_BEGIN(test_prof_threshold_hook) {
	test_skip_if(!config_stats);

	/* Test setting and reading the hook (both value and null) */
	write_prof_threshold_hook(mock_prof_threshold_hook);
	expect_ptr_eq(read_prof_threshold_hook(), mock_prof_threshold_hook, "Unexpected hook");

	write_prof_threshold_hook(NULL);
	expect_ptr_null(read_prof_threshold_hook(), "Hook was erased");

	/* Reset everything before the test */
	reset_test_config();
	write_prof_threshold_hook(mock_prof_threshold_hook);

	int err = mallctl("thread.peak.reset", NULL, NULL, NULL, 0);
	expect_d_eq(err, 0, "Peak reset failed");

	/* Note that since we run this test multiple times and we don't reset
	   the allocation counter, each time we offset the callback by the
	   amount we allocate over the threshold. */

	/* A simple small allocation is not enough to trigger the callback */
	allocate_chunks(1);
	expect_u64_eq(hook_calls, 0, "Hook not called yet");

	/* Enough allocations to trigger the callback */
	allocate_chunks(ALLOC_ITERATIONS_IN_THRESHOLD);
	expect_threshold_calls(1);

	/* Enough allocations to trigger the callback again */
	allocate_chunks(ALLOC_ITERATIONS_IN_THRESHOLD);
	expect_threshold_calls(2);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_threshold_hook);
}
