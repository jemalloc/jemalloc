#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

TEST_BEGIN(test_prof_backtrace_stats) {
	test_skip_if(!config_prof);
	test_skip_if(!config_stats);

	uint64_t oldval;
	size_t sz = sizeof(oldval);
	assert_d_eq(mallctl("stats.prof_backtrace_count", &oldval, &sz, NULL, 0),
		0, "mallctl failed");

	void *p = malloc(1);
	free(p);

	uint64_t newval;
	assert_d_eq(mallctl("stats.prof_backtrace_count", &newval, &sz, NULL, 0),
		0, "mallctl failed");

	assert_u64_eq(newval, oldval + 1, "prof_backtrace_count not incremented");
}
TEST_END

TEST_BEGIN(test_prof_stack_range_stats) {
#ifdef __linux__
    test_skip_if(!config_prof);
    test_skip_if(!config_prof_frameptr);
    test_skip_if(!config_stats);

    uint64_t oldval;
    size_t sz = sizeof(oldval);
    assert_d_eq(mallctl("stats.prof_stack_range_count", &oldval, &sz, NULL, 0),
      0, "mallctl failed");

    uintptr_t stack_end = (uintptr_t)__builtin_frame_address(0);
    prof_thread_stack_start(stack_end);

    uint64_t newval;
    assert_d_eq(mallctl("stats.prof_stack_range_count", &newval, &sz, NULL, 0),
      0, "mallctl failed");

    assert_u64_eq(newval, oldval + 1, "prof_stack_range_count not incremented");
#else
    test_skip_if(true);
#endif  // __linux__
}
TEST_END

int
main(void) {
    return test(test_prof_backtrace_stats, test_prof_stack_range_stats);
}
