#include "test/jemalloc_test.h"

TEST_BEGIN(test_basic_get)
{
	uint64_t now = malloc_time_get_ns();

	assert_u64_gt(now, 0, "Unexpected time: now=0");
}
TEST_END

TEST_BEGIN(test_monotonic)
{
	uint64_t prev = malloc_time_get_ns();
	uint64_t now;
	int i;

	for (i = 0; i < 1000000; i++) {
		now = malloc_time_get_ns();
		assert_u64_ge(now, prev,
		    "malloc_time_get_ns() is not monotonic");
		prev = now;
	}
}
TEST_END

int
main(void)
{

	return (test(
	    test_basic_get,
	    test_monotonic));
}
