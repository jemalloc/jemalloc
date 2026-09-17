#include "test/jemalloc_test.h"

#include "jemalloc/internal/util.h"

static void
test_switch_background_thread_ctl(bool new_val) {
	bool   e0, e1;
	size_t sz = sizeof(bool);

	e1 = new_val;
	expect_d_eq(mallctl("background_thread", (void *)&e0, &sz, &e1, sz), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(e0, !e1, "background_thread should be %d before.\n", !e1);
	if (e1) {
		expect_zu_gt(n_background_threads, 0,
		    "Number of background threads should be non zero.\n");
	} else {
		expect_zu_eq(n_background_threads, 0,
		    "Number of background threads should be zero.\n");
	}
}

static void
test_repeat_background_thread_ctl(bool before) {
	bool   e0, e1;
	size_t sz = sizeof(bool);

	e1 = before;
	expect_d_eq(mallctl("background_thread", (void *)&e0, &sz, &e1, sz), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(e0, before, "background_thread should be %d.\n", before);
	if (e1) {
		expect_zu_gt(n_background_threads, 0,
		    "Number of background threads should be non zero.\n");
	} else {
		expect_zu_eq(n_background_threads, 0,
		    "Number of background threads should be zero.\n");
	}
}

TEST_BEGIN(test_background_thread_ctl) {
	test_skip_if(!have_background_thread);

	bool   e0, e1;
	size_t sz = sizeof(bool);

	expect_d_eq(mallctl("opt.background_thread", (void *)&e0, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctl("background_thread", (void *)&e1, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_b_eq(
	    e0, e1, "Default and opt.background_thread does not match.\n");
	if (e0) {
		test_switch_background_thread_ctl(false);
	}
	expect_zu_eq(n_background_threads, 0,
	    "Number of background threads should be 0.\n");

	for (unsigned i = 0; i < 4; i++) {
		test_switch_background_thread_ctl(true);
		test_repeat_background_thread_ctl(true);
		test_repeat_background_thread_ctl(true);

		test_switch_background_thread_ctl(false);
		test_repeat_background_thread_ctl(false);
		test_repeat_background_thread_ctl(false);
	}
}
TEST_END

TEST_BEGIN(test_background_thread_running) {
	test_skip_if(!have_background_thread);
	test_skip_if(!config_stats);

#if defined(JEMALLOC_BACKGROUND_THREAD)
	tsd_t                    *tsd = tsd_fetch();
	background_thread_info_t *info = &background_thread_info[0];

	test_repeat_background_thread_ctl(false);
	test_switch_background_thread_ctl(true);
	expect_b_eq(info->state, background_thread_started,
	    "Background_thread did not start.\n");

	nstime_t start;
	nstime_init_update(&start);

	bool ran = false;
	while (true) {
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		if (info->tot_n_runs > 0) {
			ran = true;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		if (ran) {
			break;
		}

		nstime_t now;
		nstime_init_update(&now);
		nstime_subtract(&now, &start);
		expect_u64_lt(nstime_sec(&now), 1000,
		    "Background threads did not run for 1000 seconds.");
		sleep(1);
	}
	test_switch_background_thread_ctl(false);
#endif
}
TEST_END

TEST_BEGIN(test_background_thread_arena_reset) {
	test_skip_if(!have_background_thread);

	test_switch_background_thread_ctl(true);

	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");
	void *p = mallocx(PAGE,
	    MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena_ind));
	expect_ptr_not_null(p, "Unexpected mallocx failure");

	/*
	 * Resetting an arena takes its background thread through
	 * started -> paused -> started (background_thread_arena_reset_begin/
	 * finish).  The reset frees p, so we must not touch it afterwards.
	 */
	size_t mib[3];
	size_t miblen = sizeof(mib) / sizeof(mib[0]);
	expect_d_eq(mallctlnametomib("arena.0.reset", mib, &miblen), 0,
	    "Unexpected mallctlnametomib failure");
	mib[1] = arena_ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected arena reset failure");

#if defined(JEMALLOC_BACKGROUND_THREAD)
	background_thread_info_t *info = background_thread_info_get(arena_ind);
	tsd_t                    *tsd = tsd_fetch();
	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	background_thread_state_t st = info->state;
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	expect_d_eq((int)st, (int)background_thread_started,
	    "Arena reset must leave the background thread started, not paused");
#endif
	expect_zu_gt(n_background_threads, 0,
	    "Arena reset must not lose background threads");

	test_switch_background_thread_ctl(false);
}
TEST_END

TEST_BEGIN(test_background_thread_stats_ctl) {
	test_skip_if(!have_background_thread);
	test_skip_if(!config_stats);

	test_switch_background_thread_ctl(true);

	uint64_t epoch = 1;
	size_t   sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", (void *)&epoch, &sz, (void *)&epoch,
	                sizeof(epoch)),
	    0, "Unexpected epoch mallctl failure");

	size_t num_threads;
	sz = sizeof(num_threads);
	expect_d_eq(mallctl("stats.background_thread.num_threads",
	                (void *)&num_threads, &sz, NULL, 0),
	    0, "Unexpected stats.background_thread.num_threads failure");
	expect_zu_eq(num_threads, n_background_threads,
	    "Reported num_threads should match n_background_threads");

	uint64_t num_runs;
	sz = sizeof(num_runs);
	expect_d_eq(mallctl("stats.background_thread.num_runs",
	                (void *)&num_runs, &sz, NULL, 0),
	    0, "Unexpected stats.background_thread.num_runs failure");

	uint64_t run_interval;
	sz = sizeof(run_interval);
	expect_d_eq(mallctl("stats.background_thread.run_interval",
	                (void *)&run_interval, &sz, NULL, 0),
	    0, "Unexpected stats.background_thread.run_interval failure");

	test_switch_background_thread_ctl(false);
}
TEST_END

int
main(void) {
	/* Background_thread creation tests reentrancy naturally. */
	return test_no_reentrancy(test_background_thread_ctl,
	    test_background_thread_running, test_background_thread_arena_reset,
	    test_background_thread_stats_ctl);
}
