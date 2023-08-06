#include "test/jemalloc_test.h"
#include "test/san.h"

const char *malloc_conf = TEST_SAN_UAF_ALIGN_DISABLE;

enum {
	alloc_option_start = 0,
	use_malloc = 0,
	use_mallocx,
	alloc_option_end
};

enum {
	dalloc_option_start = 0,
	use_free = 0,
	use_dallocx,
	use_sdallocx,
	dalloc_option_end
};

static bool global_test;

static void *
alloc_func(size_t sz, unsigned alloc_option) {
	void *ret;

	switch (alloc_option) {
	case use_malloc:
		ret = malloc(sz);
		break;
	case use_mallocx:
		ret = mallocx(sz, 0);
		break;
	default:
		unreachable();
	}
	expect_ptr_not_null(ret, "Unexpected malloc / mallocx failure");

	return ret;
}

static void
dalloc_func(void *ptr, size_t sz, unsigned dalloc_option) {
	switch (dalloc_option) {
	case use_free:
		free(ptr);
		break;
	case use_dallocx:
		dallocx(ptr, 0);
		break;
	case use_sdallocx:
		sdallocx(ptr, sz, 0);
		break;
	default:
		unreachable();
	}
}

static size_t
tcache_bytes_read_global(void) {
	uint64_t epoch;
	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch,
	    sizeof(epoch)), 0, "Unexpected mallctl() failure");

	size_t tcache_bytes;
	size_t sz = sizeof(tcache_bytes);
	assert_d_eq(mallctl(
	    "stats.arenas." STRINGIFY(MALLCTL_ARENAS_ALL) ".tcache_bytes",
	    &tcache_bytes, &sz, NULL, 0), 0, "Unexpected mallctl failure");

	return tcache_bytes;
}

static size_t
tcache_bytes_read_per_thread(void) {
	size_t tcache_bytes = 0;
	tsd_t *tsd = tsd_fetch();
	tcache_t *tcache = tcache_get(tsd);
	for (szind_t i=0; i < tsd_thread_nhbins_get(tsd); i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		cache_bin_sz_t ncached =
		    cache_bin_ncached_get_internal(cache_bin);
		tcache_bytes += ncached * sz_index2size(i);
	}
	return tcache_bytes;
}
static void
tcache_bytes_check_update(size_t *prev, ssize_t diff) {
	size_t tcache_bytes = global_test ? tcache_bytes_read_global():
	    tcache_bytes_read_per_thread();
	expect_zu_eq(tcache_bytes, *prev + diff, "tcache bytes not expected");
	*prev += diff;
}

static void
test_tcache_bytes_alloc(size_t alloc_size, size_t tcache_max,
    unsigned alloc_option, unsigned dalloc_option) {
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "Unexpected tcache flush failure");

	size_t usize = sz_s2u(alloc_size);
	/* No change is expected if usize is outside of tcache_max range. */
	bool cached = (usize <= tcache_max);
	ssize_t diff = cached ? usize : 0;

	void *ptr1 = alloc_func(alloc_size, alloc_option);
	void *ptr2 = alloc_func(alloc_size, alloc_option);

	size_t bytes = global_test ? tcache_bytes_read_global() :
	    tcache_bytes_read_per_thread();
	dalloc_func(ptr2, alloc_size, dalloc_option);
	/* Expect tcache_bytes increase after dalloc */
	tcache_bytes_check_update(&bytes, diff);

	dalloc_func(ptr1, alloc_size, alloc_option);
	/* Expect tcache_bytes increase again */
	tcache_bytes_check_update(&bytes, diff);

	void *ptr3 = alloc_func(alloc_size, alloc_option);
	if (cached) {
		expect_ptr_eq(ptr1, ptr3, "Unexpected cached ptr");
	}
	/* Expect tcache_bytes decrease after alloc */
	tcache_bytes_check_update(&bytes, -diff);

	void *ptr4 = alloc_func(alloc_size, alloc_option);
	if (cached) {
		expect_ptr_eq(ptr2, ptr4, "Unexpected cached ptr");
	}
	/* Expect tcache_bytes decrease again */
	tcache_bytes_check_update(&bytes, -diff);

	dalloc_func(ptr3, alloc_size, dalloc_option);
	tcache_bytes_check_update(&bytes, diff);
	dalloc_func(ptr4, alloc_size, dalloc_option);
	tcache_bytes_check_update(&bytes, diff);
}

static void
test_tcache_max_impl(size_t target_tcache_max, unsigned alloc_option,
    unsigned dalloc_option) {
	size_t tcache_max, sz;
	sz = sizeof(tcache_max);
	if (global_test) {
		assert_d_eq(mallctl("arenas.tcache_max", (void *)&tcache_max,
		    &sz, NULL, 0), 0, "Unexpected mallctl() failure");
		expect_zu_eq(tcache_max, target_tcache_max,
		    "Global tcache_max not expected");
	} else {
		assert_d_eq(mallctl("thread.tcache.max",
		    (void *)&tcache_max, &sz, NULL,.0), 0,
		    "Unexpected.mallctl().failure");
		if (tcache_max != target_tcache_max) {
			abort();
		}
		expect_zu_eq(tcache_max, target_tcache_max,
		    "Current thread's tcache_max not expected");
	}
	test_tcache_bytes_alloc(1, tcache_max, alloc_option, dalloc_option);
	test_tcache_bytes_alloc(tcache_max - 1, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(tcache_max, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(tcache_max + 1, tcache_max, alloc_option,
	    dalloc_option);

	test_tcache_bytes_alloc(PAGE - 1, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(PAGE, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(PAGE + 1, tcache_max, alloc_option,
	    dalloc_option);

	size_t large;
	sz = sizeof(large);
	assert_d_eq(mallctl("arenas.lextent.0.size", (void *)&large, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	test_tcache_bytes_alloc(large - 1, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(large, tcache_max, alloc_option,
	    dalloc_option);
	test_tcache_bytes_alloc(large + 1, tcache_max, alloc_option,
	    dalloc_option);
}

TEST_BEGIN(test_tcache_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);
	test_skip_if(san_uaf_detection_enabled());

	unsigned arena_ind, alloc_option, dalloc_option;
	size_t sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctl("thread.arena", NULL, NULL, &arena_ind,
	    sizeof(arena_ind)), 0, "Unexpected mallctl() failure");

	global_test = true;
	for (alloc_option = alloc_option_start;
	     alloc_option < alloc_option_end;
	     alloc_option++) {
		for (dalloc_option = dalloc_option_start;
		     dalloc_option < dalloc_option_end;
		     dalloc_option++) {
			/* opt.tcache_max set to 1024 in tcache_max.sh. */
			test_tcache_max_impl(1024, alloc_option,
			    dalloc_option);
		}
	}
}
TEST_END

static size_t
tcache_max2nhbins(size_t tcache_max) {
	return sz_size2index(tcache_max) + 1;
}
static void *
tcache_check(void *arg) {
	size_t old_thread_tcache_max, new_thread_tcache_max, sz;
	unsigned thread_nhbins;
	tsd_t *tsd = tsd_fetch();
	sz = sizeof(size_t);
	new_thread_tcache_max = *(size_t *)arg;

	/* Check the default tcache_max and nhbins of each thread. */
	thread_nhbins = thread_nhbins_get(tsd);
	expect_zu_eq(thread_nhbins, (size_t)nhbins,
	    "Unexpected default value for thread_nhbins");
	assert_d_eq(mallctl("thread.tcache.max",
	    (void *)&old_thread_tcache_max, &sz,
	    (void *)&new_thread_tcache_max, sz),.0,
	    "Unexpected.mallctl().failure");
	expect_zu_eq(old_thread_tcache_max, opt_tcache_max,
	    "Unexpected default value for thread_tcache_max");
	/*
	 * After reset, check the thread's tcache_max and nhbins both through
	 * direct query and alloc tests.
	 */
	if (new_thread_tcache_max> TCACHE_MAXCLASS_LIMIT) {
		new_thread_tcache_max = TCACHE_MAXCLASS_LIMIT;
	}
	for (unsigned alloc_option = alloc_option_start;
	     alloc_option < alloc_option_end;
	     alloc_option++) {
		for (unsigned dalloc_option = dalloc_option_start;
		     dalloc_option < dalloc_option_end;
		     dalloc_option++) {
			test_tcache_max_impl(new_thread_tcache_max,
			    alloc_option, dalloc_option);
		}
	}
	thread_nhbins = thread_nhbins_get(tsd);
	expect_zu_eq(thread_nhbins, tcache_max2nhbins(new_thread_tcache_max),
	    "Unexpected value for thread_nhbins");

	return NULL;
}

TEST_BEGIN(test_thread_tcache_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);
	test_skip_if(san_uaf_detection_enabled());

	unsigned nthreads = 50;
	global_test = false;
	VARIABLE_ARRAY(thd_t, threads, nthreads);
	VARIABLE_ARRAY(size_t, all_threads_tcache_max, nthreads);
	for (unsigned i=0; i < nthreads; i++) {
		all_threads_tcache_max[i] = 1024 * (1<<(i%20));
	}
	for (unsigned i = 0; i < nthreads; i++) {
		thd_create(&threads[i], tcache_check,
		    &(all_threads_tcache_max[i]));
	}
	for (unsigned i = 0; i < nthreads; i++) {
		thd_join(threads[i], NULL);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_tcache_max,
	    test_thread_tcache_max);
}

