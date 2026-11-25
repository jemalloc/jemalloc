#include "test/jemalloc_test.h"

#include "jemalloc/internal/thread_event.h"

#include "test/prof_data.h"
#include "jemalloc/internal/prof_data.h"

TEST_BEGIN(test_prof_realloc) {
	tsd_t      *tsd;
	int         flags;
	void       *p, *q;
	prof_info_t prof_info_p, prof_info_q;
	prof_cnt_t  cnt_0, cnt_1, cnt_2, cnt_3;

	test_skip_if(!config_prof);

	tsd = tsd_fetch();
	flags = MALLOCX_TCACHE_NONE;

	prof_cnt_all(&cnt_0);
	p = mallocx(1024, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	prof_info_get(tsd, p, NULL, &prof_info_p);
	expect_ptr_ne(
	    prof_info_p.alloc_tctx, PROF_TCTX_SENTINEL, "Expected valid tctx");
	prof_cnt_all(&cnt_1);
	expect_u64_eq(cnt_0.curobjs + 1, cnt_1.curobjs,
	    "Allocation should have increased sample size");

	q = rallocx(p, 2048, flags);
	expect_ptr_ne(p, q, "Expected move");
	expect_ptr_not_null(p, "Unexpected rmallocx() failure");
	prof_info_get(tsd, q, NULL, &prof_info_q);
	expect_ptr_ne(
	    prof_info_q.alloc_tctx, PROF_TCTX_SENTINEL, "Expected valid tctx");
	prof_cnt_all(&cnt_2);
	expect_u64_eq(cnt_1.curobjs, cnt_2.curobjs,
	    "Reallocation should not have changed sample size");

	dallocx(q, flags);
	prof_cnt_all(&cnt_3);
	expect_u64_eq(cnt_0.curobjs, cnt_3.curobjs,
	    "Sample size should have returned to base level");
}
TEST_END

/*
 * Tests for experimental.prof_nmissed and experimental.prof_bytes_missed.
 *
 * prof_tctx.sh sets lg_prof_sample:0, so every allocation is due for a sample.
 * pre_reentrancy() makes the calling thread reentrant.  Counters are compared
 * as deltas, because earlier work in the process may already have moved them.
 */

typedef struct {
	size_t count;
	size_t bytes;
} missed_t;

static missed_t
missed_get(void) {
	missed_t missed = {
	    prof_sample_nmissed_get(), prof_sample_bytes_missed_get()};
	return missed;
}

static void
expect_missed_since(
    missed_t before, size_t count, size_t bytes, const char *what) {
	missed_t now = missed_get();
	expect_zu_eq(now.count - before.count, count,
	    "%s: unexpected prof_nmissed change", what);
	expect_zu_eq(now.bytes - before.bytes, bytes,
	    "%s: unexpected prof_bytes_missed change", what);
}

static bool
sampled(void *p) {
	prof_info_t info;
	prof_info_get(tsd_fetch(), p, NULL, &info);
	return prof_tctx_is_valid(info.alloc_tctx);
}

TEST_BEGIN(test_prof_missed_ctl) {
	size_t count, bytes, sz;

	sz = sizeof(count);
	int count_ret = mallctl(
	    "experimental.prof_nmissed", &count, &sz, NULL, 0);
	sz = sizeof(bytes);
	int bytes_ret = mallctl(
	    "experimental.prof_bytes_missed", &bytes, &sz, NULL, 0);
	if (!config_prof || !config_stats) {
		expect_d_eq(count_ret, ENOENT,
		    "prof_nmissed should need --enable-prof and stats");
		expect_d_eq(bytes_ret, ENOENT,
		    "prof_bytes_missed should need --enable-prof and stats");
		goto label_test_end;
	}
	assert_d_eq(count_ret, 0, "Unexpected mallctl() failure");
	assert_d_eq(bytes_ret, 0, "Unexpected mallctl() failure");
	expect_zu_eq(count, prof_sample_nmissed_get(),
	    "prof_nmissed should read the counter");
	expect_zu_eq(bytes, prof_sample_bytes_missed_get(),
	    "prof_bytes_missed should read the counter");
	expect_d_eq(mallctl("experimental.prof_nmissed", NULL, NULL, &count,
	                sizeof(count)),
	    EPERM, "prof_nmissed should be read-only");
	expect_d_eq(mallctl("experimental.prof_bytes_missed", NULL, NULL,
	                &bytes, sizeof(bytes)),
	    EPERM, "prof_bytes_missed should be read-only");

	if (opt_prof) {
		/* A new miss is visible without an epoch refresh. */
		tsd_t *tsd = tsd_fetch();
		pre_reentrancy(tsd, NULL);
		void *p = mallocx(1, 0);
		post_reentrancy(tsd);
		assert_ptr_not_null(p, "Unexpected mallocx() failure");
		size_t count_after;
		sz = sizeof(count_after);
		assert_d_eq(mallctl("experimental.prof_nmissed", &count_after,
		                &sz, NULL, 0),
		    0, "Unexpected mallctl() failure");
		expect_zu_eq(count_after - count, 1,
		    "prof_nmissed should not need an epoch refresh");
		dallocx(p, 0);
	}
}
TEST_END

TEST_BEGIN(test_prof_missed_nominal) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	missed_t before = missed_get();
	void    *p = mallocx(37, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_true(sampled(p), "A nominal allocation should be sampled");
	void *q = rallocx(p, 65, 0);
	assert_ptr_not_null(q, "Unexpected rallocx() failure");
	expect_true(sampled(q), "A nominal reallocation should be sampled");
	dallocx(q, 0);
	expect_missed_since(before, 0, 0, "sampled nominal allocations");

	/* Due but unsampled, because this thread turned sampling off. */
	bool   active, inactive = false;
	size_t sz = sizeof(active);
	assert_d_eq(mallctl("thread.prof.active", &active, &sz, &inactive,
	                sizeof(inactive)),
	    0, "Unexpected mallctl() failure");
	before = missed_get();
	p = mallocx(37, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_false(
	    sampled(p), "thread.prof.active=false should stop sampling");
	q = rallocx(p, 65, 0);
	assert_ptr_not_null(q, "Unexpected rallocx() failure");
	expect_false(
	    sampled(q), "thread.prof.active=false should stop sampling");
	dallocx(q, 0);
	expect_missed_since(before, 0, 0, "unsampled nominal allocations");
	assert_d_eq(
	    mallctl("thread.prof.active", NULL, NULL, &active, sizeof(active)),
	    0, "Unexpected mallctl() failure");
}
TEST_END

TEST_BEGIN(test_prof_missed_reentrant_malloc) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	tsd_t *tsd = tsd_fetch();
	pre_reentrancy(tsd, NULL);

	missed_t before = missed_get();
	void    *p = mallocx(37, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_false(
	    sampled(p), "A reentrant allocation should not be sampled");
	/* Counted with its usable size, not the 37 bytes requested. */
	expect_missed_since(before, 1, nallocx(37, 0), "mallocx()");

	before = missed_get();
	void *q = calloc(3, 100);
	assert_ptr_not_null(q, "Unexpected calloc() failure");
	expect_missed_since(before, 1, nallocx(300, 0), "calloc()");

	before = missed_get();
	void *r = mallocx(2 * SC_LARGE_MINCLASS, 0);
	assert_ptr_not_null(r, "Unexpected mallocx() failure");
	expect_missed_since(before, 1, sallocx(r, 0), "large mallocx()");

	post_reentrancy(tsd);

	/* Frees never decrement the counters. */
	before = missed_get();
	dallocx(p, 0);
	free(q);
	dallocx(r, 0);
	expect_missed_since(before, 0, 0, "frees");
}
TEST_END

TEST_BEGIN(test_prof_missed_reentrant_realloc) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	tsd_t *tsd = tsd_fetch();
	pre_reentrancy(tsd, NULL);
	void *p = mallocx(37, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	missed_t before = missed_get();
	void    *q = rallocx(p, 65, 0);
	assert_ptr_not_null(q, "Unexpected rallocx() failure");
	expect_ptr_ne(q, p, "Expected rallocx() to move to a larger class");
	expect_false(
	    sampled(q), "A reentrant reallocation should not be sampled");
	expect_missed_since(before, 1, nallocx(65, 0), "moving rallocx()");

	/* A same-class realloc stays in place but is still an allocation. */
	size_t same_class = nallocx(65, 0) - 1;
	before = missed_get();
	void *r = realloc(q, same_class);
	assert_ptr_not_null(r, "Unexpected realloc() failure");
	expect_ptr_eq(r, q, "Expected a same-class realloc() to stay in place");
	expect_missed_since(
	    before, 1, nallocx(same_class, 0), "same-class realloc()");

	/* Large sizes, so that the shrink works in place for any page size. */
	void *big = mallocx(2 * SC_LARGE_MINCLASS, 0);
	assert_ptr_not_null(big, "Unexpected mallocx() failure");
	before = missed_get();
	size_t resized = xallocx(big, SC_LARGE_MINCLASS, 0, 0);
	expect_zu_eq(resized, SC_LARGE_MINCLASS, "Expected an in-place shrink");
	expect_missed_since(before, 1, resized, "resizing xallocx()");

	before = missed_get();
	expect_zu_eq(xallocx(r, same_class, 0, 0), nallocx(same_class, 0),
	    "Expected xallocx() to leave the size unchanged");
	expect_missed_since(before, 0, 0, "non-resizing xallocx()");

	post_reentrancy(tsd);
	dallocx(r, 0);
	dallocx(big, 0);
}
TEST_END

TEST_BEGIN(test_prof_missed_failed) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);
	/* Only a 64-bit address space reliably fails to map this size. */
	test_skip_if(LG_SIZEOF_PTR != 3);

	tsd_t *tsd = tsd_fetch();
	pre_reentrancy(tsd, NULL);
	void *p = mallocx(64, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	missed_t        before = missed_get();
	volatile size_t too_large = SC_LARGE_MAXCLASS;
	expect_ptr_null(mallocx(too_large, 0), "Expected mallocx() to fail");
	expect_ptr_null(rallocx(p, too_large, 0), "Expected rallocx() to fail");
	expect_missed_since(before, 0, 0, "failed allocations");

	post_reentrancy(tsd);
	dallocx(p, 0);
}
TEST_END

static uint64_t
sample_wait_1024(tsd_t *tsd) {
	(void)tsd;
	return 1024;
}

TEST_BEGIN(test_prof_missed_threshold) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	/* Restart this thread's sample countdown at exactly 1024 bytes. */
	tsd_t *tsd = tsd_fetch();
	uint64_t (*new_wait)(tsd_t *) = prof_sample_te_handler.new_event_wait;
	prof_sample_te_handler.new_event_wait = &sample_wait_1024;
	tsd_te_init(tsd);

	pre_reentrancy(tsd, NULL);
	missed_t before = missed_get();
	void    *p = mallocx(16, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_missed_since(
	    before, 0, 0, "allocation before the sample is due");
	void *q = mallocx(2048, 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	expect_false(
	    sampled(q), "A reentrant allocation should not be sampled");
	/* Only the allocation the sample falls in, not the earlier 16 bytes. */
	expect_missed_since(
	    before, 1, nallocx(2048, 0), "allocation the sample falls in");
	post_reentrancy(tsd);

	prof_sample_te_handler.new_event_wait = new_wait;
	tsd_te_init(tsd);
	dallocx(p, 0);
	dallocx(q, 0);
}
TEST_END

TEST_BEGIN(test_prof_missed_inactive) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	tsd_t   *tsd = tsd_fetch();
	bool     active = prof_active_set(tsd_tsdn(tsd), false);
	missed_t before = missed_get();
	pre_reentrancy(tsd, NULL);
	void *p = mallocx(4096, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	void *q = rallocx(p, 8192, 0);
	assert_ptr_not_null(q, "Unexpected rallocx() failure");
	post_reentrancy(tsd);
	prof_active_set(tsd_tsdn(tsd), active);
	expect_missed_since(
	    before, 0, 0, "reentrant allocations while inactive");
	dallocx(q, 0);

	/* prof.reset makes the thread re-create its profiling data. */
	before = missed_get();
	p = mallocx(4096, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0), 0,
	    "Unexpected prof.reset failure");
	q = mallocx(4096, 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	expect_true(sampled(q), "Sampling should resume after prof.reset");
	dallocx(p, 0);
	dallocx(q, 0);
	expect_missed_since(before, 0, 0, "prof.reset");
}
TEST_END

typedef struct {
	missed_t delta;
	unsigned state;
	int      reentrancy;
} reincarnated_result_t;

static void *
reincarnated_alloc_thd(void *arg) {
	reincarnated_result_t *result = (reincarnated_result_t *)arg;
	tsd_t                 *tsd = tsd_fetch();

	/* Run this thread's TSD cleanup early; the TSD enters purgatory. */
	tsd_cleanup(tsd);
	missed_t before = missed_get();
	/* The next allocation reincarnates it (tsd_data_init_nocleanup()). */
	void *p = mallocx(16, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	result->state = tsd_state_get(tsd);
	result->reentrancy = tsd_reentrancy_level_get(tsd);
	missed_t after = missed_get();
	result->delta.count = after.count - before.count;
	result->delta.bytes = after.bytes - before.bytes;
	dallocx(p, 0);
	return NULL;
}

TEST_BEGIN(test_prof_missed_reincarnated) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	reincarnated_result_t result;
	thd_t                 thd;
	thd_create(&thd, reincarnated_alloc_thd, &result);
	thd_join(thd, NULL);
	expect_u_eq(result.state, tsd_state_reincarnated,
	    "Expected the allocation to reincarnate the TSD");
	expect_d_gt(
	    result.reentrancy, 0, "A reincarnated TSD should be reentrant");
	expect_zu_eq(result.delta.count, 1,
	    "An allocation on a reincarnated TSD should be counted");
	expect_zu_eq(result.delta.bytes, nallocx(16, 0),
	    "An allocation on a reincarnated TSD should add its usable size");
}
TEST_END

#define MISSED_NTHREADS 4
#define MISSED_NALLOCS 1000

static void *
reentrant_alloc_thd(void *arg) {
	(void)arg;
	tsd_t *tsd = tsd_fetch();
	pre_reentrancy(tsd, NULL);
	for (unsigned i = 0; i < MISSED_NALLOCS; i++) {
		void *p = mallocx(64, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() failure");
		dallocx(p, 0);
	}
	post_reentrancy(tsd);
	return NULL;
}

TEST_BEGIN(test_prof_missed_concurrent) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	missed_t before = missed_get();
	thd_t    thds[MISSED_NTHREADS];
	for (unsigned i = 0; i < MISSED_NTHREADS; i++) {
		thd_create(&thds[i], reentrant_alloc_thd, NULL);
	}
	for (unsigned i = 0; i < MISSED_NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
	size_t n = (size_t)MISSED_NTHREADS * MISSED_NALLOCS;
	expect_missed_since(
	    before, n, n * nallocx(64, 0), "concurrent reentrant allocations");
}
TEST_END

typedef struct {
	char   buf[16384];
	size_t len;
} stats_out_t;

static void
stats_out_write(void *opaque, const char *s) {
	stats_out_t *out = (stats_out_t *)opaque;
	size_t       n = strlen(s);
	assert_zu_lt(out->len + n, sizeof(out->buf), "Stats output too long");
	memcpy(out->buf + out->len, s, n + 1);
	out->len += n;
}

/* Returns SIZE_MAX if the key is missing from the JSON "stats" object. */
static size_t
json_stats_value(const char *json, const char *key) {
	const char *stats = strstr(json, "\"stats\":{");
	expect_ptr_not_null(stats, "No \"stats\" object in the JSON output");
	if (stats == NULL) {
		return SIZE_MAX;
	}
	char pattern[64];
	malloc_snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *value = strstr(stats, pattern);
	expect_ptr_not_null(
	    value, "No \"%s\" in the JSON \"stats\" object", key);
	if (value == NULL) {
		return SIZE_MAX;
	}
	return (size_t)strtoull(value + strlen(pattern), NULL, 10);
}

TEST_BEGIN(test_prof_missed_stats_print) {
	test_skip_if(!config_prof || !config_stats || !opt_prof);

	/* Make sure there is at least one miss to print. */
	tsd_t *tsd = tsd_fetch();
	pre_reentrancy(tsd, NULL);
	void *p = mallocx(64, 0);
	post_reentrancy(tsd);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	size_t count, bytes, sz;
	sz = sizeof(count);
	assert_d_eq(mallctl("experimental.prof_nmissed", &count, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	sz = sizeof(bytes);
	assert_d_eq(
	    mallctl("experimental.prof_bytes_missed", &bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	assert_zu_gt(count, 0, "Expected at least one miss");

	/* Global stats only: no general info, arenas, or mutexes. */
	static stats_out_t out;
	out.len = 0;
	out.buf[0] = '\0';
	malloc_stats_print(stats_out_write, &out, "Jgmdax");
	expect_zu_eq(json_stats_value(out.buf, "prof_nmissed"), count,
	    "JSON prof_nmissed should match the mallctl");
	expect_zu_eq(json_stats_value(out.buf, "prof_bytes_missed"), bytes,
	    "JSON prof_bytes_missed should match the mallctl");

	out.len = 0;
	out.buf[0] = '\0';
	malloc_stats_print(stats_out_write, &out, "gmdax");
	char line[128];
	malloc_snprintf(line, sizeof(line),
	    "Allocations escaped from profiling: %zu\n", count);
	expect_ptr_not_null(strstr(out.buf, line), "Missing text: %s", line);
	malloc_snprintf(line, sizeof(line),
	    "Usable bytes escaped from profiling: %zu\n", bytes);
	expect_ptr_not_null(strstr(out.buf, line), "Missing text: %s", line);

	dallocx(p, 0);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_prof_realloc, test_prof_missed_ctl,
	    test_prof_missed_nominal, test_prof_missed_reentrant_malloc,
	    test_prof_missed_reentrant_realloc, test_prof_missed_failed,
	    test_prof_missed_threshold, test_prof_missed_inactive,
	    test_prof_missed_reincarnated, test_prof_missed_concurrent,
	    test_prof_missed_stats_print);
}
