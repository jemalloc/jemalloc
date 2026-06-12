#include "test/jemalloc_test.h"

static int  data_cleanup_count;

void
data_cleanup(int *data) {
	if (data_cleanup_count == 0) {
		expect_x_eq(*data, MALLOC_TSD_TEST_DATA_INIT,
		    "Argument passed into cleanup function should match tsd "
		    "value");
	}
	++data_cleanup_count;

	/*
	 * Allocate during cleanup for two rounds, in order to assure that
	 * jemalloc's internal tsd reinitialization happens.
	 */
	bool reincarnate = false;
	switch (*data) {
	case MALLOC_TSD_TEST_DATA_INIT:
		*data = 1;
		reincarnate = true;
		break;
	case 1:
		*data = 2;
		reincarnate = true;
		break;
	case 2:
		return;
	default:
		not_reached();
	}

	if (reincarnate) {
		void *p = mallocx(1, 0);
		expect_ptr_not_null(p, "Unexpeced mallocx() failure");
		dallocx(p, 0);
	}
}

static void *
thd_start(void *arg) {
	int   d = (int)(uintptr_t)arg;
	void *p;

	/*
	 * Test free before tsd init -- the free fast path (which does not
	 * explicitly check for NULL) has to tolerate this case, and fall back
	 * to free_default.
	 */
	free(NULL);

	tsd_t *tsd = tsd_fetch();
	expect_x_eq(tsd_test_data_get(tsd), MALLOC_TSD_TEST_DATA_INIT,
	    "Initial tsd get should return initialization value");

	p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	tsd_test_data_set(tsd, d);
	expect_x_eq(tsd_test_data_get(tsd), d,
	    "After tsd set, tsd get should return value that was set");

	d = 0;
	expect_x_eq(tsd_test_data_get(tsd), (int)(uintptr_t)arg,
	    "Resetting local data should have no effect on tsd");

	tsd_test_callback_set(tsd, &data_cleanup);

	free(p);
	return NULL;
}

TEST_BEGIN(test_tsd_main_thread) {
	thd_start((void *)(uintptr_t)0xa5f3e329);
}
TEST_END

TEST_BEGIN(test_tsd_sub_thread) {
	thd_t thd;

	data_cleanup_count = 0;
	thd_create(&thd, thd_start, (void *)MALLOC_TSD_TEST_DATA_INIT);
	thd_join(thd, NULL);
	/*
	 * We reincarnate twice in the data cleanup, so it should execute at
	 * least 3 times.
	 */
	expect_x_ge(data_cleanup_count, 3,
	    "Cleanup function should have executed multiple times.");
}
TEST_END

static void *
thd_start_reincarnated(void *arg) {
	tsd_t *tsd = tsd_fetch();
	assert(tsd);

	void *p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	/* Manually trigger reincarnation. */
	expect_ptr_not_null(tsd_arena_get(tsd), "Should have tsd arena set.");
	tsd_cleanup((void *)tsd);
	expect_ptr_null(
	    *tsd_arenap_get_unsafe(tsd), "TSD arena should have been cleared.");
	expect_u_eq(tsd_state_get(tsd), tsd_state_purgatory,
	    "TSD state should be purgatory\n");

	free(p);
	expect_u_eq(tsd_state_get(tsd), tsd_state_reincarnated,
	    "TSD state should be reincarnated\n");
	p = mallocx(1, MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	expect_ptr_null(*tsd_arenap_get_unsafe(tsd),
	    "Should not have tsd arena set after reincarnation.");

	free(p);
	tsd_cleanup((void *)tsd);
	expect_ptr_null(*tsd_arenap_get_unsafe(tsd),
	    "TSD arena should have been cleared after 2nd cleanup.");

	return NULL;
}

TEST_BEGIN(test_tsd_reincarnation) {
	thd_t thd;
	thd_create(&thd, thd_start_reincarnated, NULL);
	thd_join(thd, NULL);
}
TEST_END

static bool tsd_bootstrap_reentrant_hook_ran;
static unsigned tsd_bootstrap_reentrant_hook_runs;

static void
tsd_bootstrap_reentrant_hook(void) {
	tsd_bootstrap_reentrant_hook_ran = true;
	tsd_bootstrap_reentrant_hook_runs++;
	test_hooks_tsd_bootstrap_hook = NULL;

	void *p = malloc(16);
	expect_ptr_not_null(p, "Unexpected recursive malloc() failure");
	free(p);
}

static void *
thd_start_reentrant_tsd_bootstrap(void *arg) {
	(void)arg;

	test_hooks_tsd_bootstrap_hook = tsd_bootstrap_reentrant_hook;
	void *p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	free(p);
	test_hooks_tsd_bootstrap_hook = NULL;

	expect_true(tsd_bootstrap_reentrant_hook_ran,
	    "TSD bootstrap hook should have executed");
	return NULL;
}

static void *
thd_start_reentrant_tsd_bootstrap_minimal(void *arg) {
	(void)arg;

	tsd_t *tsd = tsd_fetch_min();
	expect_u_eq(tsd_state_get(tsd), tsd_state_minimal_initialized,
	    "TSD should be minimal initialized");

	test_hooks_tsd_bootstrap_hook = tsd_bootstrap_reentrant_hook;
	void *p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	free(p);
	test_hooks_tsd_bootstrap_hook = NULL;

	expect_true(tsd_bootstrap_reentrant_hook_ran,
	    "TSD bootstrap hook should have executed");
	return NULL;
}

TEST_BEGIN(test_tsd_reentrant_bootstrap) {
	thd_t thd;

	tsd_bootstrap_reentrant_hook_ran = false;
	tsd_bootstrap_reentrant_hook_runs = 0;
	thd_create(&thd, thd_start_reentrant_tsd_bootstrap, NULL);
	thd_join(thd, NULL);

	tsd_bootstrap_reentrant_hook_ran = false;
	thd_create(&thd, thd_start_reentrant_tsd_bootstrap_minimal, NULL);
	thd_join(thd, NULL);

	expect_u_eq(tsd_bootstrap_reentrant_hook_runs, 2,
	    "TSD bootstrap hook should have executed once per case");
}
TEST_END

static void *
thd_start_dalloc_only(void *arg) {
	void **ptrs = (void **)arg;

	tsd_t *tsd = tsd_fetch_min();
	if (tsd_state_get(tsd) != tsd_state_minimal_initialized) {
		/* Allocation happened implicitly. */
		expect_u_eq(tsd_state_get(tsd), tsd_state_nominal,
		    "TSD state should be nominal");
		return NULL;
	}

	void *ptr;
	for (size_t i = 0; (ptr = ptrs[i]) != NULL; i++) {
		/* Offset by 1 because of the manual tsd_fetch_min above. */
		if (i + 1 < TSD_MIN_INIT_STATE_MAX_FETCHED) {
			expect_u_eq(tsd_state_get(tsd),
			    tsd_state_minimal_initialized,
			    "TSD should be minimal initialized");
		} else {
			/* State may be nominal or nominal_slow. */
			expect_true(tsd_nominal(tsd), "TSD should be nominal");
		}
		free(ptr);
	}

	return NULL;
}

static void
test_sub_thread_n_dalloc(size_t nptrs) {
	void **ptrs = (void **)malloc(sizeof(void *) * (nptrs + 1));
	for (size_t i = 0; i < nptrs; i++) {
		ptrs[i] = malloc(8);
	}
	ptrs[nptrs] = NULL;

	thd_t thd;
	thd_create(&thd, thd_start_dalloc_only, (void *)ptrs);
	thd_join(thd, NULL);
	free(ptrs);
}

TEST_BEGIN(test_tsd_sub_thread_dalloc_only) {
	test_sub_thread_n_dalloc(1);
	test_sub_thread_n_dalloc(16);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED - 2);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED - 1);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED + 1);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED + 2);
	test_sub_thread_n_dalloc(TSD_MIN_INIT_STATE_MAX_FETCHED * 2);
}
TEST_END

#if !defined(JEMALLOC_MALLOC_THREAD_CLEANUP) && !defined(JEMALLOC_TLS) \
    && !defined(_WIN32)
#define TEST_TSD_AFTER_TEARDOWN
static const bool skip_tsd_after_teardown = false;
#else
static const bool skip_tsd_after_teardown = true;
#endif

static void *
tsd_raw_get(void) {
#ifdef TEST_TSD_AFTER_TEARDOWN
	return pthread_getspecific(tsd_tsd);
#else
	return NULL;
#endif
}

static bool
tsd_raw_clear(void) {
#ifdef TEST_TSD_AFTER_TEARDOWN
	return pthread_setspecific(tsd_tsd, NULL) == 0;
#else
	return true;
#endif
}

static void
tsd_teardown_for_test(void) {
	tsd_t *tsd = tsd_fetch();
	void *tsd_wrapper = tsd_raw_get();
	expect_ptr_not_null(tsd_wrapper, "TSD wrapper should exist after malloc");

	/*
	 * Emulate a jemalloc call after the pthread-key destructor has finished
	 * with the generic TSD wrapper.  This is the state seen by cleanup code
	 * that runs late in thread teardown on pthread_getspecific()-based
	 * builds: the first cleanup call moves a nominal TSD to purgatory, where
	 * deallocation is handled by the existing reincarnation path; the second
	 * cleanup call emulates the final destructor round, after which the
	 * wrapper is released and no TSD remains to reincarnate.
	 */
	tsd_cleanup(tsd);
	tsd_cleanup(tsd);
	malloc_tsd_dalloc(tsd_wrapper);
	expect_true(tsd_raw_clear(), "Unexpected TSD key clear failure");
	expect_ptr_null(tsd_raw_get(),
	    "TSD key should be clear before the late jemalloc call");
}

static void *
thd_start_free_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");
	void *free_victim = malloc(32);
	expect_ptr_not_null(free_victim, "Unexpected malloc() failure");
	void *dallocx_victim = mallocx(32, 0);
	expect_ptr_not_null(dallocx_victim, "Unexpected mallocx() failure");
	void *sdallocx_victim = mallocx(32, 0);
	expect_ptr_not_null(sdallocx_victim, "Unexpected mallocx() failure");

	tsd_teardown_for_test();

	free(free_victim);
	expect_ptr_null(tsd_raw_get(),
	    "free() after TSD teardown must not re-create TSD");
	dallocx(dallocx_victim, 0);
	expect_ptr_null(tsd_raw_get(),
	    "dallocx() after TSD teardown must not re-create TSD");
	sdallocx(sdallocx_victim, 32, 0);
	expect_ptr_null(tsd_raw_get(),
	    "sdallocx() after TSD teardown must not re-create TSD");

	free(keep);
	return NULL;
}

TEST_BEGIN(test_tsd_free_after_teardown) {
	test_skip_if(skip_tsd_after_teardown);

	thd_t thd;
	thd_create(&thd, thd_start_free_after_teardown, NULL);
	thd_join(thd, NULL);
}
TEST_END

static void
expect_tsd_recreated(void *p, const char *func) {
	expect_ptr_not_null(p, "%s() after TSD teardown should still allocate",
	    func);
	expect_ptr_not_null(tsd_raw_get(),
	    "%s() after TSD teardown should preserve TSD reincarnation", func);
}

static void *
thd_start_malloc_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *p = malloc(32);
	expect_tsd_recreated(p, "malloc");
	if (p != NULL) {
		free(p);
	}

	free(keep);
	return NULL;
}

static void *
thd_start_mallocx_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *p = mallocx(32, 0);
	expect_tsd_recreated(p, "mallocx");
	if (p != NULL) {
		dallocx(p, 0);
	}

	free(keep);
	return NULL;
}

static void *
thd_start_calloc_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *p = calloc(1, 32);
	expect_tsd_recreated(p, "calloc");
	if (p != NULL) {
		free(p);
	}

	free(keep);
	return NULL;
}

static void *
thd_start_aligned_alloc_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *p = aligned_alloc(sizeof(void *), 32);
	expect_tsd_recreated(p, "aligned_alloc");
	if (p != NULL) {
		free(p);
	}

	free(keep);
	return NULL;
}

static void *
thd_start_posix_memalign_after_teardown(void *arg) {
	(void)arg;

	void *keep = malloc(32);
	expect_ptr_not_null(keep, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *p = NULL;
	expect_d_eq(posix_memalign(&p, sizeof(void *), 32), 0,
	    "posix_memalign() after TSD teardown should still allocate");
	expect_tsd_recreated(p, "posix_memalign");
	if (p != NULL) {
		free(p);
	}

	free(keep);
	return NULL;
}

static void *
thd_start_realloc_after_teardown(void *arg) {
	(void)arg;

	void *p = malloc(32);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	void *ret = realloc(p, 64);
	expect_tsd_recreated(ret, "realloc");
	if (ret != NULL) {
		p = ret;
	}
	free(p);
	return NULL;
}

static void *
thd_start_rallocx_after_teardown(void *arg) {
	(void)arg;

	void *p = mallocx(32, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	tsd_teardown_for_test();

	void *ret = rallocx(p, 64, 0);
	expect_tsd_recreated(ret, "rallocx");
	if (ret != NULL) {
		p = ret;
	}
	dallocx(p, 0);
	return NULL;
}

static void *
thd_start_xallocx_after_teardown(void *arg) {
	(void)arg;

	void *p = mallocx(32, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	size_t old_usize = sallocx(p, 0);

	tsd_teardown_for_test();

	expect_zu_ge(xallocx(p, 64, 0, 0), old_usize,
	    "xallocx() after TSD teardown should preserve behavior");
	expect_ptr_not_null(tsd_raw_get(),
	    "xallocx() after TSD teardown should preserve TSD reincarnation");
	dallocx(p, 0);
	return NULL;
}

static void *
thd_start_realloc_zero_after_teardown(void *arg) {
	(void)arg;

	void *p = malloc(32);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	tsd_teardown_for_test();

	if (opt_zero_realloc_action == zero_realloc_action_abort) {
		free(p);
		expect_ptr_null(tsd_raw_get(),
		    "free() after TSD teardown must not re-create TSD");
		return NULL;
	}

	void *ret = realloc(p, 0);
	if (opt_zero_realloc_action == zero_realloc_action_free) {
		expect_ptr_null(ret, "realloc(ptr, 0) should return NULL");
		expect_ptr_null(tsd_raw_get(),
		    "realloc(ptr, 0) after TSD teardown must not re-create TSD");
	} else {
		expect_tsd_recreated(ret, "realloc(ptr, 0)");
		if (ret != NULL) {
			p = ret;
		}
		free(p);
	}
	return NULL;
}

TEST_BEGIN(test_tsd_malloc_after_teardown) {
	test_skip_if(skip_tsd_after_teardown);

	thd_t thd;
	thd_create(&thd, thd_start_malloc_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_mallocx_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_calloc_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_aligned_alloc_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_posix_memalign_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_realloc_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_rallocx_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_xallocx_after_teardown, NULL);
	thd_join(thd, NULL);
	thd_create(&thd, thd_start_realloc_zero_after_teardown, NULL);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	/* Ensure tsd bootstrapped. */
	if (nallocx(1, 0) == 0) {
		malloc_printf("Initialization error");
		return test_status_fail;
	}

	return test_no_reentrancy(test_tsd_main_thread, test_tsd_sub_thread,
	    test_tsd_sub_thread_dalloc_only, test_tsd_reincarnation,
	    test_tsd_reentrant_bootstrap,
	    test_tsd_free_after_teardown, test_tsd_malloc_after_teardown);
}
