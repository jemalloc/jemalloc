#include "test/jemalloc_test.h"

#include "jemalloc/internal/tcache_ncached_target.h"

TEST_BEGIN(test_tcache_ncached_target_bounds) {
	const struct {
		cache_bin_sz_t ncached_max;
		cache_bin_sz_t target_min;
		cache_bin_sz_t target_max;
		cache_bin_sz_t retain_after_overflow;
		cache_bin_sz_t flush_remain;
	} cases[] = {
		{1, 1, 1, 1, 0},
		{2, 2, 2, 2, 1},
		{3, 2, 2, 2, 2},
		{4, 2, 2, 2, 2},
		{8, 2, 4, 4, 4},
		{20, 2, 10, 10, 10},
		{64, 2, 32, 32, 32},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		const cache_bin_sz_t ncached_max = cases[i].ncached_max;
		expect_zu_eq(cases[i].target_min,
		    tcache_ncached_target_min(ncached_max),
		    "Unexpected minimum for capacity %u", (unsigned)ncached_max);
		expect_zu_eq(cases[i].target_max,
		    tcache_ncached_target_max(ncached_max),
		    "Unexpected maximum for capacity %u", (unsigned)ncached_max);
		cache_bin_sz_t retain_after_overflow =
		    tcache_ncached_retain_after_overflow(
		        ncached_max, ncached_max);
		expect_zu_eq(cases[i].retain_after_overflow,
		    retain_after_overflow,
		    "Unexpected overflow retention for capacity %u",
		    (unsigned)ncached_max);
		expect_zu_eq(cases[i].flush_remain,
		    tcache_ncached_flush_remain(
		        retain_after_overflow, ncached_max),
		    "Unexpected flush remainder for capacity %u",
		    (unsigned)ncached_max);
	}
}
TEST_END

TEST_BEGIN(test_tcache_ncached_fill_lifecycle) {
	const cache_bin_sz_t ncached_max = 64;
	cache_bin_sz_t target = tcache_ncached_target_max(ncached_max);
	expect_zu_eq(32, target, "Unexpected initial fill target");

	target = tcache_ncached_fill_after_underuse(target, ncached_max);
	expect_zu_eq(16, target, "Underuse should halve the fill target");
	target = tcache_ncached_fill_after_underuse(target, ncached_max);
	expect_zu_eq(8, target, "Repeated underuse should halve it again");

	target = tcache_ncached_fill_after_refill(target, ncached_max);
	expect_zu_eq(16, target, "A refill should double the fill target");
	target = tcache_ncached_fill_after_refill(target, ncached_max);
	expect_zu_eq(32, target, "Refills should restore the maximum");
	target = tcache_ncached_fill_after_refill(target, ncached_max);
	expect_zu_eq(32, target, "The fill target should remain capped");

	target = 2;
	target = tcache_ncached_fill_after_underuse(target, ncached_max);
	expect_zu_eq(2, target, "The fill target should remain above its floor");
}
TEST_END

TEST_BEGIN(test_tcache_ncached_retain_after_gc) {
	const struct {
		cache_bin_sz_t ncached;
		cache_bin_sz_t low_water;
		cache_bin_sz_t ncached_max;
		cache_bin_sz_t expected;
		const char    *description;
	} cases[] = {
		{40, 40, 64, 2, "Nothing was consumed"},
		{40, 39, 64, 2, "One item was consumed"},
		{40, 32, 64, 10, "Eight items were consumed"},
		{40, 20, 64, 25, "Twenty items were consumed"},
		{64, 1, 64, 64, "The target saturates at capacity"},
	};

	for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
		expect_zu_eq(cases[i].expected,
		    tcache_ncached_retain_after_gc(cases[i].ncached,
		        cases[i].low_water, cases[i].ncached_max),
		    "%s", cases[i].description);
	}
}
TEST_END

TEST_BEGIN(test_tcache_ncached_retain_lifecycle) {
	const cache_bin_sz_t ncached_max = 64;
	cache_bin_sz_t target = 2;

	target = tcache_ncached_retain_after_refill(target, 20, ncached_max);
	expect_zu_eq(40, target,
	    "A refill should provide room for another equal refill");
	target = tcache_ncached_retain_after_refill(target, 4, ncached_max);
	expect_zu_eq(40, target, "A smaller refill should not lower the target");
	target = tcache_ncached_retain_after_overflow(target, ncached_max);
	expect_zu_eq(20, target, "An overflow should halve the retention target");
	target = tcache_ncached_retain_after_overflow(target, ncached_max);
	expect_zu_eq(10, target,
	    "Repeated overflow should halve the retention target again");
	target = tcache_ncached_retain_after_refill(target, 33, ncached_max);
	expect_zu_eq(64, target, "The retention target should cap at capacity");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_tcache_ncached_target_bounds,
	    test_tcache_ncached_fill_lifecycle,
	    test_tcache_ncached_retain_after_gc,
	    test_tcache_ncached_retain_lifecycle);
}
