#include "test/jemalloc_test.h"
#include "test/san.h"

const char *malloc_conf = TEST_SAN_UAF_ALIGN_DISABLE;
extern void tcache_bin_info_compute(
    cache_bin_info_t tcache_bin_info[TCACHE_NBINS_MAX]);

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
tcache_bytes_read_local(void) {
	size_t tcache_bytes = 0;
	tsd_t *tsd = tsd_fetch();
	tcache_t *tcache = tcache_get(tsd);
	for (szind_t i = 0; i < tcache_nbins_get(tcache->tcache_slow); i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (tcache_bin_disabled(i, cache_bin, tcache->tcache_slow)) {
			continue;
		}
		cache_bin_sz_t ncached = cache_bin_ncached_get_local(cache_bin,
		    &cache_bin->bin_info);
		tcache_bytes += ncached * sz_index2size(i);
	}
	return tcache_bytes;
}
static void
tcache_bytes_check_update(size_t *prev, ssize_t diff) {
	size_t tcache_bytes = global_test ? tcache_bytes_read_global():
	    tcache_bytes_read_local();
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
	    tcache_bytes_read_local();
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
	global_test = false;
}
TEST_END

static size_t
tcache_max2nbins(size_t tcache_max) {
	return sz_size2index(tcache_max) + 1;
}

static void
validate_tcache_stack(tcache_t *tcache) {
	/* Assume bins[0] is enabled. */
	void *tcache_stack = tcache->bins[0].stack_head;
	bool expect_found = cache_bin_stack_use_thp() ? true : false;

	/* Walk through all blocks to see if the stack is within range. */
	base_t *base = b0get();
	base_block_t *next = base->blocks;
	bool found = false;
	do {
		base_block_t *block = next;
		if ((byte_t *)tcache_stack >= (byte_t *)block &&
		    (byte_t *)tcache_stack < ((byte_t *)block + block->size)) {
			found = true;
			break;
		}
		next = block->next;
	} while (next != NULL);

	expect_true(found == expect_found, "Unexpected tcache stack source");
}

static void *
tcache_check(void *arg) {
	size_t old_tcache_max, new_tcache_max, min_tcache_max, sz;
	unsigned tcache_nbins;
	tsd_t *tsd = tsd_fetch();
	tcache_t *tcache = tsd_tcachep_get(tsd);
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	sz = sizeof(size_t);
	new_tcache_max = *(size_t *)arg;
	min_tcache_max = 1;

	/*
	 * Check the default tcache_max and tcache_nbins of each thread's
	 * auto tcache.
	 */
	old_tcache_max = tcache_max_get(tcache_slow);
	expect_zu_eq(old_tcache_max, opt_tcache_max,
	    "Unexpected default value for tcache_max");
	tcache_nbins = tcache_nbins_get(tcache_slow);
	expect_zu_eq(tcache_nbins, (size_t)global_do_not_change_tcache_nbins,
	    "Unexpected default value for tcache_nbins");
	validate_tcache_stack(tcache);

	/*
	 * Close the tcache and test the set.
	 * Test an input that is not a valid size class, it should be ceiled
	 * to a valid size class.
	 */
	bool e0 = false, e1;
	size_t bool_sz = sizeof(bool);
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)&e1, &bool_sz,
	    (void *)&e0, bool_sz), 0, "Unexpected mallctl() error");
	expect_true(e1, "Unexpected previous tcache state");

	size_t temp_tcache_max = TCACHE_MAXCLASS_LIMIT - 1;
	assert_d_eq(mallctl("thread.tcache.max",
	    NULL, NULL, (void *)&temp_tcache_max, sz),.0,
	    "Unexpected.mallctl().failure");
	old_tcache_max = tcache_max_get(tcache_slow);
	expect_zu_eq(old_tcache_max, TCACHE_MAXCLASS_LIMIT,
	    "Unexpected value for tcache_max");
	tcache_nbins = tcache_nbins_get(tcache_slow);
	expect_zu_eq(tcache_nbins, TCACHE_NBINS_MAX,
	    "Unexpected value for tcache_nbins");
	assert_d_eq(mallctl("thread.tcache.max",
	    (void *)&old_tcache_max, &sz,
	    (void *)&min_tcache_max, sz),.0,
	    "Unexpected.mallctl().failure");
	expect_zu_eq(old_tcache_max, TCACHE_MAXCLASS_LIMIT,
	    "Unexpected value for tcache_max");

	/* Enable tcache, the set should still be valid. */
	e0 = true;
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)&e1, &bool_sz,
	    (void *)&e0, bool_sz), 0, "Unexpected mallctl() error");
	expect_false(e1, "Unexpected previous tcache state");
	min_tcache_max = sz_s2u(min_tcache_max);
	expect_zu_eq(tcache_max_get(tcache_slow), min_tcache_max,
	    "Unexpected value for tcache_max");
	expect_zu_eq(tcache_nbins_get(tcache_slow),
	    tcache_max2nbins(min_tcache_max), "Unexpected value for nbins");
	assert_d_eq(mallctl("thread.tcache.max",
	    (void *)&old_tcache_max, &sz,
	    (void *)&new_tcache_max, sz),.0,
	    "Unexpected.mallctl().failure");
	expect_zu_eq(old_tcache_max, min_tcache_max,
	    "Unexpected value for tcache_max");
	validate_tcache_stack(tcache);

	/*
	 * Check the thread's tcache_max and nbins both through mallctl
	 * and alloc tests.
	 */
	if (new_tcache_max > TCACHE_MAXCLASS_LIMIT) {
		new_tcache_max = TCACHE_MAXCLASS_LIMIT;
	}
	old_tcache_max = tcache_max_get(tcache_slow);
	expect_zu_eq(old_tcache_max, new_tcache_max,
	    "Unexpected value for tcache_max");
	tcache_nbins = tcache_nbins_get(tcache_slow);
	expect_zu_eq(tcache_nbins, tcache_max2nbins(new_tcache_max),
	    "Unexpected value for tcache_nbins");
	for (unsigned alloc_option = alloc_option_start;
	     alloc_option < alloc_option_end;
	     alloc_option++) {
		for (unsigned dalloc_option = dalloc_option_start;
		     dalloc_option < dalloc_option_end;
		     dalloc_option++) {
			test_tcache_max_impl(new_tcache_max,
			    alloc_option, dalloc_option);
		}
		validate_tcache_stack(tcache);
	}

	return NULL;
}

TEST_BEGIN(test_thread_tcache_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);
	test_skip_if(san_uaf_detection_enabled());

	unsigned nthreads = 8;
	global_test = false;
	VARIABLE_ARRAY(thd_t, threads, nthreads);
	VARIABLE_ARRAY(size_t, all_threads_tcache_max, nthreads);
	for (unsigned i = 0; i < nthreads; i++) {
		all_threads_tcache_max[i] = 1024 * (1<<((i + 10) % 20));
		if (i == nthreads - 1) {
			all_threads_tcache_max[i] = UINT_MAX;
		}
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

static void
check_bins_info(cache_bin_info_t tcache_bin_info[TCACHE_NBINS_MAX]) {
	size_t mib_get[4], mib_get_len;
	mib_get_len = sizeof(mib_get) / sizeof(size_t);
	const char *get_name = "thread.tcache.ncached_max.read_sizeclass";
	size_t ncached_max;
	size_t sz = sizeof(size_t);
	expect_d_eq(mallctlnametomib(get_name, mib_get, &mib_get_len), 0,
	    "Unexpected mallctlnametomib() failure");

	for (szind_t i = 0; i < TCACHE_NBINS_MAX; i++) {
		size_t bin_size = sz_index2size(i);
		expect_d_eq(mallctlbymib(mib_get, mib_get_len,
		    (void *)&ncached_max, &sz,
		    (void *)&bin_size, sizeof(size_t)), 0,
		    "Unexpected mallctlbymib() failure");
		expect_zu_eq(ncached_max, tcache_bin_info[i].ncached_max,
		    "Unexpected ncached_max for bin %d", i);
		/* Check ncached_max returned under a non-bin size. */
		bin_size--;
		size_t temp_ncached_max = 0;
		expect_d_eq(mallctlbymib(mib_get, mib_get_len,
		    (void *)&temp_ncached_max, &sz,
		    (void *)&bin_size, sizeof(size_t)), 0,
		    "Unexpected mallctlbymib() failure");
		expect_zu_eq(temp_ncached_max, ncached_max,
		    "Unexpected ncached_max for inaccurate bin size.");
	}
}

static void *
ncached_max_check(void* args) {
	cache_bin_info_t tcache_bin_info[TCACHE_NBINS_MAX];
	cache_bin_info_t tcache_bin_info_backup[TCACHE_NBINS_MAX];
	tsd_t *tsd = tsd_fetch();
	tcache_t *tcache = tsd_tcachep_get(tsd);
	assert(tcache != NULL);
	tcache_slow_t *tcache_slow = tcache->tcache_slow;

	/* Check the initial bin settings. */
	tcache_bin_info_compute(tcache_bin_info);
	memcpy(tcache_bin_info_backup, tcache_bin_info,
	    sizeof(tcache_bin_info));
	unsigned nbins = tcache_nbins_get(tcache_slow);
	for (szind_t i = nbins; i < TCACHE_NBINS_MAX; i++) {
		cache_bin_info_init(&tcache_bin_info[i], 0);
	}
	check_bins_info(tcache_bin_info);

	size_t mib_set[4], mib_set_len;
	mib_set_len = sizeof(mib_set) / sizeof(size_t);
	const char *set_name = "thread.tcache.ncached_max.write";
	expect_d_eq(mallctlnametomib(set_name, mib_set, &mib_set_len), 0,
	    "Unexpected mallctlnametomib() failure");

	/* Test the ncached_max set with tcache on. */
	char inputs[100] = "8-128:1|160-160:11|170-320:22|224-8388609:0";
	char *inputp = inputs;
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), 0,
	    "Unexpected mallctlbymib() failure");
	for (szind_t i = 0; i < TCACHE_NBINS_MAX; i++) {
		if (i >= sz_size2index(8) &&i <= sz_size2index(128)) {
			cache_bin_info_init(&tcache_bin_info[i], 1);
		}
		if (i == sz_size2index(160)) {
			cache_bin_info_init(&tcache_bin_info[i], 11);
		}
		if (i >= sz_size2index(170) && i <= sz_size2index(320)) {
			cache_bin_info_init(&tcache_bin_info[i], 22);
		}
		if (i >= sz_size2index(224)) {
			cache_bin_info_init(&tcache_bin_info[i], 0);
		}
		if (i >= nbins) {
			cache_bin_info_init(&tcache_bin_info[i], 0);
		}
	}
	check_bins_info(tcache_bin_info);

	/*
	 * Close the tcache and set ncached_max of some bins.  It will be
	 * set properly but thread.tcache.ncached_max.read still returns 0
	 * since the bin is not available yet.  After enabling the tcache,
	 * the new setting will not be carried on.  Instead, the default
	 * settings will be applied.
	 */
	bool e0 = false, e1;
	size_t bool_sz = sizeof(bool);
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)&e1, &bool_sz,
	    (void *)&e0, bool_sz), 0, "Unexpected mallctl() error");
	expect_true(e1, "Unexpected previous tcache state");
	strcpy(inputs, "0-112:8");
	/* Setting returns ENOENT when the tcache is disabled. */
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), ENOENT,
	    "Unexpected mallctlbymib() failure");
	/* All ncached_max should return 0 once tcache is disabled. */
	for (szind_t i = 0; i < TCACHE_NBINS_MAX; i++) {
		cache_bin_info_init(&tcache_bin_info[i], 0);
	}
	check_bins_info(tcache_bin_info);

	e0 = true;
	expect_d_eq(mallctl("thread.tcache.enabled", (void *)&e1, &bool_sz,
	    (void *)&e0, bool_sz), 0, "Unexpected mallctl() error");
	expect_false(e1, "Unexpected previous tcache state");
	memcpy(tcache_bin_info, tcache_bin_info_backup,
	    sizeof(tcache_bin_info_backup));
	for (szind_t i = tcache_nbins_get(tcache_slow); i < TCACHE_NBINS_MAX;
	    i++) {
		cache_bin_info_init(&tcache_bin_info[i], 0);
	}
	check_bins_info(tcache_bin_info);

	/*
	 * Set ncached_max of bins not enabled yet.  Then, enable them by
	 * resetting tcache_max.  The ncached_max changes should stay.
	 */
	size_t tcache_max = 1024;
	assert_d_eq(mallctl("thread.tcache.max",
	    NULL, NULL, (void *)&tcache_max, sizeof(size_t)),.0,
	    "Unexpected.mallctl().failure");
	for (szind_t i = sz_size2index(1024) + 1; i < TCACHE_NBINS_MAX; i++) {
		cache_bin_info_init(&tcache_bin_info[i], 0);
	}
	strcpy(inputs, "2048-6144:123");
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), 0,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);

	tcache_max = 6144;
	assert_d_eq(mallctl("thread.tcache.max",
	    NULL, NULL, (void *)&tcache_max, sizeof(size_t)),.0,
	    "Unexpected.mallctl().failure");
	memcpy(tcache_bin_info, tcache_bin_info_backup,
	    sizeof(tcache_bin_info_backup));
	for (szind_t i = sz_size2index(2048); i < TCACHE_NBINS_MAX; i++) {
		if (i <= sz_size2index(6144)) {
			cache_bin_info_init(&tcache_bin_info[i], 123);
		} else if (i > sz_size2index(6144)) {
			cache_bin_info_init(&tcache_bin_info[i], 0);
		}
	}
	check_bins_info(tcache_bin_info);

	/* Test an empty input, it should do nothing. */
	strcpy(inputs, "");
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), 0,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);

	/* Test a half-done string, it should return EINVAL and do nothing. */
	strcpy(inputs, "4-1024:7|256-1024");
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), EINVAL,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);

	/*
	 * Test an invalid string with start size larger than end size.  It
	 * should return success but do nothing.
	 */
	strcpy(inputs, "1024-256:7");
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), 0,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);

	/*
	 * Test a string exceeding the length limit, it should return EINVAL
	 * and do nothing.
	 */
	char *long_inputs = (char *)malloc(10000 * sizeof(char));
	expect_true(long_inputs != NULL, "Unexpected allocation failure.");
	for (int i = 0; i < 200; i++) {
		memcpy(long_inputs + i * 9, "4-1024:3|", 9);
	}
	memcpy(long_inputs + 200 * 9, "4-1024:3", 8);
	long_inputs[200 * 9 + 8] = '\0';
	inputp = long_inputs;
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), EINVAL,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);
	free(long_inputs);

	/*
	 * Test a string with invalid characters, it should return EINVAL
	 * and do nothing.
	 */
	strcpy(inputs, "k8-1024:77p");
	inputp = inputs;
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), EINVAL,
	    "Unexpected mallctlbymib() failure");
	check_bins_info(tcache_bin_info);

	/* Test large ncached_max, it should return success but capped. */
	strcpy(inputs, "1024-1024:65540");
	expect_d_eq(mallctlbymib(mib_set, mib_set_len, NULL, NULL,
	    (void *)&inputp, sizeof(char *)), 0,
	    "Unexpected mallctlbymib() failure");
	cache_bin_info_init(&tcache_bin_info[sz_size2index(1024)],
	    CACHE_BIN_NCACHED_MAX);
	check_bins_info(tcache_bin_info);

	return NULL;
}

TEST_BEGIN(test_ncached_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(san_uaf_detection_enabled());
	unsigned nthreads = 8;
	VARIABLE_ARRAY(thd_t, threads, nthreads);
	for (unsigned i = 0; i < nthreads; i++) {
		thd_create(&threads[i], ncached_max_check, NULL);
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
	    test_thread_tcache_max,
	    test_ncached_max);
}

