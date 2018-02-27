#include "test/jemalloc_test.h"

const char *malloc_conf = "background_thread:false,narenas:1";

TEST_BEGIN(test_deferred) {
	test_skip_if(!have_background_thread);

	unsigned id;
	size_t sz_u = sizeof(unsigned);

	/*
	 * 10 here is somewhat arbitrary, except insofar as we want to ensure
	 * that the number of background threads is smaller than the number of
	 * arenas.  I'll ragequit long before we have to spin up 10 threads per
	 * cpu to handle background purging, so this is a conservative
	 * approximation.
	 */
	for (unsigned i = 0; i < 10 * ncpus; i++) {
		assert_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool enable = true;
	size_t sz_b = sizeof(bool);
	assert_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_deferred);
}
