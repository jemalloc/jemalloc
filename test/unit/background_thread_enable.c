#include "test/jemalloc_test.h"

const char *malloc_conf =
    "background_thread:false,narenas:1,max_background_threads:8";

static unsigned
max_test_narenas(void) {
	/*
	 * 10 here is somewhat arbitrary, except insofar as we want to ensure
	 * that the number of background threads is smaller than the number of
	 * arenas.  I'll ragequit long before we have to spin up 10 threads per
	 * cpu to handle background purging, so this is a conservative
	 * approximation.
	 */
	unsigned ret = 10 * ncpus;

	/* Limit the max to avoid VM exhaustion on 32-bit . */
	return ret > 256 ? 256 : ret;
}

TEST_BEGIN(test_deferred) {
	test_skip_if(!have_background_thread);

	unsigned id;
	size_t   sz_u = sizeof(unsigned);

	for (unsigned i = 0; i < max_test_narenas(); i++) {
		expect_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool   enable = true;
	size_t sz_b = sizeof(bool);
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	enable = false;
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to disable background threads");
}
TEST_END

TEST_BEGIN(test_max_background_threads) {
	test_skip_if(!have_background_thread);

	size_t max_n_thds;
	size_t opt_max_n_thds;
	size_t sz_m = sizeof(max_n_thds);
	expect_d_eq(mallctl("opt.max_background_threads", &opt_max_n_thds,
	                &sz_m, NULL, 0),
	    0, "Failed to get opt.max_background_threads");
	expect_d_eq(
	    mallctl("max_background_threads", &max_n_thds, &sz_m, NULL, 0), 0,
	    "Failed to get max background threads");
	expect_zu_eq(opt_max_n_thds, max_n_thds,
	    "max_background_threads and "
	    "opt.max_background_threads should match");
	expect_d_eq(
	    mallctl("max_background_threads", NULL, NULL, &max_n_thds, sz_m), 0,
	    "Failed to set max background threads");
	size_t size_zero = 0;
	expect_d_ne(
	    mallctl("max_background_threads", NULL, NULL, &size_zero, sz_m), 0,
	    "Should not allow zero background threads");

	unsigned id;
	size_t   sz_u = sizeof(unsigned);

	for (unsigned i = 0; i < max_test_narenas(); i++) {
		expect_d_eq(mallctl("arenas.create", &id, &sz_u, NULL, 0), 0,
		    "Failed to create arena");
	}

	bool   enable = true;
	size_t sz_b = sizeof(bool);
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz_b), 0,
	    "Failed to enable background threads");
	expect_zu_eq(n_background_threads, max_n_thds,
	    "Number of background threads should not change.\n");
	size_t new_max_thds = max_n_thds - 1;
	if (new_max_thds > 0) {
		expect_d_eq(mallctl("max_background_threads", NULL, NULL,
		                &new_max_thds, sz_m),
		    0, "Failed to set max background threads");
		expect_zu_eq(n_background_threads, new_max_thds,
		    "Number of background threads should decrease by 1.\n");
	}
	new_max_thds = 1;
	expect_d_eq(
	    mallctl("max_background_threads", NULL, NULL, &new_max_thds, sz_m),
	    0, "Failed to set max background threads");
	expect_d_ne(
	    mallctl("max_background_threads", NULL, NULL, &size_zero, sz_m), 0,
	    "Should not allow zero background threads");
	expect_zu_eq(n_background_threads, new_max_thds,
	    "Number of background threads should be 1.\n");
}
TEST_END

static atomic_b_t stress_stop;

static void
set_background_thread(bool enable) {
	size_t sz = sizeof(bool);
	expect_d_eq(mallctl("background_thread", NULL, NULL, &enable, sz), 0,
	    "Failed to set background_thread");
}

static void *
stress_worker(void *arg) {
	(void)arg;
	while (!atomic_load_b(&stress_stop, ATOMIC_RELAXED)) {
		void *p = mallocx(PAGE, MALLOCX_TCACHE_NONE);
		if (p != NULL) {
			dallocx(p, MALLOCX_TCACHE_NONE);
		}
	}
	return NULL;
}

TEST_BEGIN(test_toggle_stress_with_concurrent_alloc) {
	test_skip_if(!have_background_thread);

	atomic_store_b(&stress_stop, false, ATOMIC_RELAXED);
	thd_t thd;
	thd_create(&thd, stress_worker, NULL);

	for (unsigned i = 0; i < 200; i++) {
		set_background_thread(true);
		expect_zu_gt(n_background_threads, 0,
		    "Background threads should be non-zero after enable "
		    "(cycle %u)", i);
		set_background_thread(false);
		expect_zu_eq(n_background_threads, 0,
		    "Background threads should be zero after disable "
		    "(cycle %u)", i);
	}

	atomic_store_b(&stress_stop, true, ATOMIC_RELAXED);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_deferred, test_max_background_threads,
	    test_toggle_stress_with_concurrent_alloc);
}
