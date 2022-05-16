#include "test/jemalloc_test.h"
#include "test/arena_util.h"

#include "jemalloc/internal/ccache.h"

static void
assert_preconditions() {
	assert_true(ccache_is_empty_unsafe(),
	    "Empty ccache precondition failed");
}

static void
flush_ccache() {
	ccache_full_flush_unsafe();
	assert_true(ccache_is_empty_unsafe(),
	    "Non-empty ccache after flushing");
}

TEST_BEGIN(test_ccache_alloc_free_noflush) {
	test_skip_if(!config_cpu_cache);
	test_skip_if(!opt_ccache);
	/*
	 * If opt_prof is on, it's possible that the allocated pointer is
	 * sampled, it will come from a different bin and be different from the
	 * freed pointer.
	 */
	test_skip_if(opt_prof);

	assert_preconditions();

	size_t alloc_size = sz_index2size(ccache_minind);
	void *initial_ptr = mallocx(alloc_size, MALLOCX_TCACHE_NONE);

	assert_true(ccache_is_empty_unsafe(),
	    "Ccache refilled after TCACHE_NONE allocation");

	/* Free and alloc one */
	free(initial_ptr);

	expect_d_eq(ccache_ncached_elements_unsafe(), 1,
	    "Free didn't put pointer to the ccache");

	void *ptr = malloc(alloc_size);
	expect_ptr_eq(ptr, initial_ptr,
	    "Allocated pointer is different from the cached one");

	assert_true(ccache_is_empty_unsafe(),
	    "Allocated pointer wasn't removed from the ccache");

	flush_ccache();
}
TEST_END

TEST_BEGIN(test_ccache_alloc_free_flush) {
	test_skip_if(!config_cpu_cache);
	test_skip_if(!opt_ccache);
	test_skip_if(!config_stats);

	assert_preconditions();

	size_t alloc_size = sz_index2size(ccache_minind);
	const int nallocs = 10000;
	void *ptr[nallocs];

	uint64_t fills_before = ccache_nfills_get();
	for (int i = 0; i < nallocs; ++i) {
		ptr[i] = malloc(alloc_size);
		expect_ptr_not_null(ptr[i],
		    "Unable to allocate from cpu cache on step %d", i);
	}
	uint64_t fills_after = ccache_nfills_get();
	expect_u64_gt(fills_after, fills_before,
	    "Expected at least one refill after %d allocations", nallocs);

	uint64_t flushes_before = ccache_nflushes_get();
	for (int i = 0; i < nallocs; ++i) {
		free(ptr[i]);
	}
	uint64_t flushes_after = ccache_nflushes_get();
	expect_u64_gt(flushes_after, flushes_before,
	    "Expected at least one flush after %d deallocations", nallocs);

	flush_ccache();
}
TEST_END

typedef struct {
	void **ptrs;
	int first;
	unsigned n;
	size_t size;
	uint64_t accumulated;
} thread_ctx_t;

static void *
thd_alloc_write(void *arg) {
	thread_ctx_t *ctx = (thread_ctx_t *)arg;
	for (unsigned i = ctx->first; i < ctx->first + ctx->n; ++i) {
		ctx->ptrs[i] = malloc(ctx->size);
		*(unsigned *)ctx->ptrs[i] = i;
	}
	return NULL;
}

static void *
thd_accumulate_free(void *arg) {
	/* Bootstrap tcache, otherwise 'free' would bypass ccache */
	void *volatile ptr = malloc(1);
	free(ptr);

	thread_ctx_t *ctx = (thread_ctx_t *)arg;
	uint64_t accumulated = 0;
	for (unsigned i = ctx->first; i < ctx->first + ctx->n; ++i) {
		accumulated += *(unsigned *)ctx->ptrs[i];
		free(ctx->ptrs[i]);
	}
	ctx->accumulated = accumulated;
	return NULL;
}

#define PRNG_SEED 0xEB134AA0BAF956C0LL

static void
random_shuffle(void **a, unsigned n) {
	uint64_t prng_state = PRNG_SEED;
	for (unsigned i = 0; i < n; ++i) {
		unsigned j = i + (unsigned)prng_range_u64(&prng_state, n - i);
		assert(j <= n);
		void *tmp = a[i];
		a[i] = a[j];
		a[j] = tmp;
	}
}

#undef PRNG_SEED

static uint64_t
global_stats_active_get() {
	uint64_t epoch = 1;
	expect_d_eq(
	    mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Failed to move the epoch");

	uint64_t res;
	size_t len = sizeof(res);
	expect_d_eq(
	    mallctl("stats.active", &res, &len, NULL, 0), 0,
	    "Failed to get stats.active via mallctl");
	return res;
}

void
run_fuzzy_test(szind_t szind) {
	assert_preconditions();

	const size_t alloc_size = sz_index2size(szind);
	const unsigned nthreads = ncpus * 4;
	const unsigned nptrs = 200000;
	const unsigned jobs_per_thread = nptrs / nthreads;

	void **ptrs = (void **)mallocx(nptrs * sizeof(void *),
	    MALLOCX_TCACHE_NONE);
	thread_ctx_t *tctx =
	    (thread_ctx_t *)mallocx(nthreads * sizeof(thread_ctx_t),
	    MALLOCX_TCACHE_NONE);

	int first_job = 0;
	for (unsigned i = 0; i < nthreads; ++i) {
		tctx[i].ptrs = ptrs;
		tctx[i].size = alloc_size;
		tctx[i].first = first_job;
		if (i == nthreads - 1) {
			tctx[i].n = nptrs - first_job;
		} else {
			tctx[i].n = jobs_per_thread;
		}
		first_job += jobs_per_thread;
	}
	thd_t *thds = (thd_t *)mallocx(nthreads * sizeof(thd_t),
	    MALLOCX_TCACHE_NONE);

	uint64_t active_before_alloc = global_stats_active_get();
	uint64_t fills_before = ccache_nfills_get();

	for (unsigned i = 0; i < nthreads; ++i) {
		thd_create(&thds[i], thd_alloc_write, (void *)&tctx[i]);
	}
	for (unsigned i = 0; i < nthreads; ++i) {
		thd_join(thds[i], NULL);
	}

	uint64_t fills_after = ccache_nfills_get();
	if (szind < SC_NBINS) {
		expect_u64_gt(fills_after, fills_before,
		    "Never filled the ccache after the large number of allocs");
	} else {
		expect_u64_eq(fills_after, fills_before,
		    "Refilled ccache bin for a large alloc");
	}

	uint64_t active_after_alloc = global_stats_active_get();
	expect_u64_gt(active_after_alloc, active_before_alloc,
	    "Active didn't grow after ccache allocations");
	expect_u64_ge(active_after_alloc - active_before_alloc,
	    nptrs * alloc_size,
	    "Insufficient amount of memory allocated to satisfy the requests");

	uint64_t sum = 0;
	uint64_t expected = (uint64_t)nptrs * (nptrs - 1) / 2;
	for (unsigned i = 0; i < nptrs; ++i) {
		sum += *(unsigned *)ptrs[i];
	}
	expect_u64_eq(sum, expected, "Sanity check sum is incorrect");

	random_shuffle(ptrs, nptrs);

	uint64_t flushes_before = ccache_nflushes_get();
	for (unsigned i = 0; i < nthreads; ++i) {
		tctx[i].accumulated = 0;
		thd_create(&thds[i], thd_accumulate_free, (void *)&tctx[i]);
	}

	sum = 0;
	expected = (uint64_t)nptrs * (nptrs - 1) / 2;
	for (unsigned i = 0; i < nthreads; ++i) {
		thd_join(thds[i], NULL);
		sum += tctx[i].accumulated;
	}
	expect_u64_eq(sum, expected, "The resulting sum is incorrect");

	uint64_t flushes_after = ccache_nflushes_get();
	expect_u64_gt(flushes_after, flushes_before,
	    "Never flushed the ccache after the large number of deallocs");

	flush_ccache();

	uint64_t active_after_free = global_stats_active_get();
	expect_u64_eq(active_after_free, active_before_alloc,
	    "Active stat did not drop back after deallocations and flushes");

	dallocx(thds, MALLOCX_TCACHE_NONE);
	dallocx(tctx, MALLOCX_TCACHE_NONE);
	dallocx(ptrs, MALLOCX_TCACHE_NONE);

}

/*
 * Fuzzy tests that makes nthreads allocate concurrently, then shuffles the
 * resulting array and makes them free concurrently.
 */
TEST_BEGIN(test_ccache_fuzzy) {
	test_skip_if(!config_cpu_cache);
	test_skip_if(!opt_ccache);
	test_skip_if(!config_stats);

	const szind_t alloc_sizeinds[] = {ccache_minind,
		(ccache_minind + ccache_maxind) / 2,
		ccache_maxind - 1};
	const unsigned nsizes = sizeof(alloc_sizeinds) / sizeof(szind_t);

	for (unsigned run = 0; run < nsizes; ++run) {
		run_fuzzy_test(alloc_sizeinds[run]);
	}
}
TEST_END

TEST_BEGIN(test_ccache_stats) {
	test_skip_if(!config_cpu_cache);
	test_skip_if(!opt_ccache);
	test_skip_if(!config_stats);

	assert_preconditions();

	const size_t alloc_size = sz_index2size(ccache_minind);
	uint64_t fills_before = ccache_nfills_get();
	void *ptr = malloc(alloc_size);
	uint64_t fills_after = ccache_nfills_get();
	if (!opt_prof) {
		expect_u64_eq(fills_after - fills_before, 1,
		    "Expected one refill after allocating from an empty "
		    "ccache");
	}
	free(ptr);

	flush_ccache();

	uint64_t flushes_before = ccache_nflushes_get();
	for (unsigned i = 0; i < CCACHE_BIN_ELEMENTS + 1; ++i) {
		void *ptr = mallocx(alloc_size, MALLOCX_TCACHE_NONE);
		free(ptr);
	}
	uint64_t flushes_after = ccache_nflushes_get();
	/*
	 * If opt_prof is on, some allocations could have been promoted to a
	 * large size class and put into a different bin.
	 */
	if (!opt_prof) {
		expect_u64_eq(flushes_after - flushes_before, 1,
		    "Expected one flush after overflowing the bin "
		    "with pointers");
	}

	flush_ccache();
}
TEST_END

int
main(void) {
	return test(
	    test_ccache_alloc_free_noflush,
	    test_ccache_alloc_free_flush,
	    test_ccache_fuzzy,
	    test_ccache_stats);
}
