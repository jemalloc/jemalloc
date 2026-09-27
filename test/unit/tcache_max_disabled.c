#include "test/jemalloc_test.h"

TEST_BEGIN(test_tcache_max_disabled) {
	test_skip_if(opt_tcache);

	tsd_t *tsd = tsd_fetch();
	assert_ptr_null(tsd_tcachep_get(tsd)->tcache_slow,
	    "Tcache should not have been initialized");

	size_t max = 0, sz = sizeof(max);
	size_t new_max = opt_tcache_max == 1024 ? 2048 : 1024;
	expect_d_eq(mallctl("thread.tcache.max", &max, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	expect_zu_eq(max, sz_s2u(opt_tcache_max),
	    "Unexpected default tcache max");
	expect_d_eq(mallctl("thread.tcache.max", NULL, NULL, &new_max,
	    sizeof(new_max)), 0, "Unexpected mallctl failure");
	expect_d_eq(mallctl("thread.tcache.max", &max, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");
	expect_zu_eq(max, new_max, "Tcache max should change while disabled");
	expect_false(tsd_tcache_enabled_get(tsd),
	    "Changing tcache max should not enable the tcache");
	expect_ptr_null(tsd_tcachep_get(tsd)->tcache_slow,
	    "Changing tcache max should not initialize the tcache");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_tcache_max_disabled);
}
