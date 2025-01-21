#include "test/jemalloc_test.h"

#include "jemalloc/internal/peak_demand.h"

TEST_BEGIN(test_peak_demand_init) {
	peak_demand_t peak_demand;
	/*
	 * Exact value doesn't matter here as we don't advance epoch in this
	 * test.
	 */
	uint64_t interval_ms = 1000;
	peak_demand_init(&peak_demand, interval_ms);

	expect_zu_eq(peak_demand_nactive_max(&peak_demand), 0,
	    "Unexpected ndirty_max value after initialization");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_basic) {
	peak_demand_t peak_demand;
	/* Make each bucket exactly one second to simplify math. */
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;

	nstime_init2(&now, /* sec */ 0, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 1024);

	nstime_init2(&now, /* sec */ 1, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 512);

	nstime_init2(&now, /* sec */ 2, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 256);

	expect_zu_eq(peak_demand_nactive_max(&peak_demand), 1024, "");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_skip_epochs) {
	peak_demand_t peak_demand;
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;

	nstime_init2(&now, /* sec */ 0, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 1024);

	nstime_init2(&now, /* sec */ PEAK_DEMAND_NBUCKETS - 1, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 512);

	nstime_init2(&now, /* sec */ 2 * (PEAK_DEMAND_NBUCKETS - 1),
	    /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 256);

	/*
	 * Updates are not evenly spread over time.  When we update at
	 * 2 * (PEAK_DEMAND_NBUCKETS - 1) second, 1024 value is already out of
	 * sliding window, but 512 is still present.
	 */
	expect_zu_eq(peak_demand_nactive_max(&peak_demand), 512, "");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_rewrite_optimization) {
	peak_demand_t peak_demand;
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;

	nstime_init2(&now, /* sec */ 0, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 1024);

	nstime_init2(&now, /* sec */ 0, /* nsec */ UINT64_MAX);
	/*
	 * This update should take reasonable time if optimization is working
	 * correctly, otherwise we'll loop from 0 to UINT64_MAX and this test
	 * will take a long time to finish.
	 */
	peak_demand_update(&peak_demand, &now, /* nactive */ 512);

	expect_zu_eq(peak_demand_nactive_max(&peak_demand), 512, "");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_out_of_interval) {
	peak_demand_t peak_demand;
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;

	nstime_init2(&now, /* sec */ 0 * PEAK_DEMAND_NBUCKETS, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 1024);

	nstime_init2(&now, /* sec */ 1 * PEAK_DEMAND_NBUCKETS, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 512);

	nstime_init2(&now, /* sec */ 2 * PEAK_DEMAND_NBUCKETS, /* nsec */ 0);
	peak_demand_update(&peak_demand, &now, /* nactive */ 256);

	/*
	 * Updates frequency is lower than tracking interval, so we should
	 * have only last value.
	 */
	expect_zu_eq(peak_demand_nactive_max(&peak_demand), 256, "");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_static_epoch) {
	peak_demand_t peak_demand;
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;
	nstime_init_zero(&now);

	/* Big enough value to overwrite values in circular buffer. */
	size_t nactive_max = 2 * PEAK_DEMAND_NBUCKETS;
	for (size_t nactive = 0; nactive <= nactive_max; ++nactive) {
		/*
		 * We should override value in the same bucket as now value
		 * doesn't change between iterations.
		 */
		peak_demand_update(&peak_demand, &now, nactive);
	}

	expect_zu_eq(peak_demand_nactive_max(&peak_demand), nactive_max, "");
}
TEST_END

TEST_BEGIN(test_peak_demand_update_epoch_advance) {
	peak_demand_t peak_demand;
	uint64_t interval_ms = 1000 * PEAK_DEMAND_NBUCKETS;
	peak_demand_init(&peak_demand, interval_ms);

	nstime_t now;
	/* Big enough value to overwrite values in circular buffer. */
	size_t nactive_max = 2 * PEAK_DEMAND_NBUCKETS;
	for (size_t nactive = 0; nactive <= nactive_max; ++nactive) {
		uint64_t sec = nactive;
		nstime_init2(&now, sec, /* nsec */ 0);
		peak_demand_update(&peak_demand, &now, nactive);
	}

	expect_zu_eq(peak_demand_nactive_max(&peak_demand), nactive_max, "");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_peak_demand_init,
	    test_peak_demand_update_basic,
	    test_peak_demand_update_skip_epochs,
	    test_peak_demand_update_rewrite_optimization,
	    test_peak_demand_update_out_of_interval,
	    test_peak_demand_update_static_epoch,
	    test_peak_demand_update_epoch_advance);
}
