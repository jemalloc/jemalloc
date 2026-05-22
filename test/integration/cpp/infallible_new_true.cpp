#include <stdio.h>

#include "test/jemalloc_test.h"

/*
 * We can't test C++ in unit tests.  In order to intercept abort, use the
 * internal test hook in integration tests.
 */
bool fake_abort_called;
void
fake_abort(const char *message) {
	const char *expected_start = "<jemalloc>: Allocation of size";
	if (strncmp(message, expected_start, strlen(expected_start)) != 0) {
		abort();
	}
	fake_abort_called = true;
}

static bool
own_operator_new(void) {
	uint64_t before, after;
	size_t   sz = sizeof(before);

	/* thread.allocated is always available, even w/o config_stats. */
	expect_d_eq(mallctl("thread.allocated", (void *)&before, &sz, NULL, 0),
	    0, "Unexpected mallctl failure reading stats");
	void *volatile ptr = ::operator new((size_t)8);
	expect_ptr_not_null(ptr, "Unexpected allocation failure");
	expect_d_eq(mallctl("thread.allocated", (void *)&after, &sz, NULL, 0),
	    0, "Unexpected mallctl failure reading stats");

	return (after != before);
}

TEST_BEGIN(test_failing_alloc) {
	test_hooks_safety_check_abort = &fake_abort;

	/*
	 * Not owning operator new is only expected to happen on MinGW which
	 * does not support operator new / delete replacement.
	 */
#ifdef _WIN32
	test_skip_if(!own_operator_new());
#else
	expect_true(own_operator_new(), "No operator new overload");
#endif
	void *volatile ptr = (void *)1;
	try {
		/* Too big of an allocation to succeed. */
		ptr = ::operator new((size_t)-1);
	} catch (...) {
		abort();
	}
	expect_ptr_null(ptr, "Allocation should have failed");
	expect_b_eq(fake_abort_called, true, "Abort hook not invoked");
	test_hooks_safety_check_abort = NULL;
}
TEST_END

int
main(void) {
	return test(test_failing_alloc);
}
