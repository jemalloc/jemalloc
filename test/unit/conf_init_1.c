#include "test/jemalloc_test.h"

const char *malloc_conf =
    "dirty_decay_ms:1234,tcache_nslots_small_max:65535";

TEST_BEGIN(test_malloc_conf_dirty_decay_ms) {
#ifdef _WIN32
	test_skip("not supported on win32");
#endif

	ssize_t dirty_decay_ms;
	size_t sz = sizeof(dirty_decay_ms);

	int err = mallctl("opt.dirty_decay_ms", &dirty_decay_ms, &sz, NULL, 0);
	assert_d_eq(err, 0, "Unexpected mallctl failure");
	expect_zd_eq(dirty_decay_ms, 1234,
	    "dirty_decay_ms should be 1234 (set via malloc_conf)");
}
TEST_END

TEST_BEGIN(test_malloc_conf_tcache_nslots_small_max) {
#ifdef _WIN32
	test_skip("not supported on win32");
#endif

	unsigned tcache_nslots_small_max;
	size_t sz = sizeof(tcache_nslots_small_max);

	int err = mallctl("opt.tcache_nslots_small_max",
	    &tcache_nslots_small_max, &sz, NULL, 0);
	assert_d_eq(err, 0, "Unexpected mallctl failure");
	expect_zu_eq(tcache_nslots_small_max, CACHE_BIN_NCACHED_MAX,
	    "tcache_nslots_small_max should match the cache-bin limit");
}
TEST_END

int
main(void) {
	return test(test_malloc_conf_dirty_decay_ms,
	    test_malloc_conf_tcache_nslots_small_max);
}
