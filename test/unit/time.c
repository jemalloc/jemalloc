#include "test/jemalloc_test.h"

TEST_BEGIN(test_time_update)
{
	struct timespec ts;

	memset(&ts, 0, sizeof(struct timespec));

	assert_false(time_update(&ts), "Basic time update failed.");

	/* Only Rip Van Winkle sleeps this long. */
	ts.tv_sec += 631152000;
	assert_true(time_update(&ts), "Update should detect time roll-back.");
}
TEST_END

int
main(void)
{

	return (test(
	    test_time_update));
}
