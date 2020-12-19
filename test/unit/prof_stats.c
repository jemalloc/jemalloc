#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_stats.h"

static void
test_wrapper(szind_t ind) {
#define N_PTRS 3
	assert(opt_prof && opt_prof_stats);

	tsd_t *tsd = tsd_fetch();

	prof_stats_t live_stats_orig;
	prof_stats_get_live(tsd, ind, &live_stats_orig);
	prof_stats_t accum_stats_orig;
	prof_stats_get_accum(tsd, ind, &accum_stats_orig);

	void *ptrs[N_PTRS];

	uint64_t live_req_sum = 0;
	uint64_t live_count = 0;
	uint64_t accum_req_sum = 0;
	uint64_t accum_count = 0;

	for (size_t i = 0, sz = sz_index2size(ind) - N_PTRS; i < N_PTRS;
	    ++i, ++sz) {
		void *p = malloc(sz);
		assert_ptr_not_null(p, "malloc() failed");
		ptrs[i] = p;
		live_req_sum += sz;
		live_count++;
		accum_req_sum += sz;
		accum_count++;
		prof_stats_t live_stats;
		prof_stats_get_live(tsd, ind, &live_stats);
		expect_u64_eq(live_stats.req_sum - live_stats_orig.req_sum,
		    live_req_sum, "");
		expect_u64_eq(live_stats.count - live_stats_orig.count,
		    live_count, "");
		prof_stats_t accum_stats;
		prof_stats_get_accum(tsd, ind, &accum_stats);
		expect_u64_eq(accum_stats.req_sum - accum_stats_orig.req_sum,
		    accum_req_sum, "");
		expect_u64_eq(accum_stats.count - accum_stats_orig.count,
		    accum_count, "");
	}

	for (size_t i = 0, sz = sz_index2size(ind) - N_PTRS; i < N_PTRS;
	    ++i, ++sz) {
		free(ptrs[i]);
		live_req_sum -= sz;
		live_count--;
		prof_stats_t live_stats;
		prof_stats_get_live(tsd, ind, &live_stats);
		expect_u64_eq(live_stats.req_sum - live_stats_orig.req_sum,
		    live_req_sum, "");
		expect_u64_eq(live_stats.count - live_stats_orig.count,
		    live_count, "");
		prof_stats_t accum_stats;
		prof_stats_get_accum(tsd, ind, &accum_stats);
		expect_u64_eq(accum_stats.req_sum - accum_stats_orig.req_sum,
		    accum_req_sum, "");
		expect_u64_eq(accum_stats.count - accum_stats_orig.count,
		    accum_count, "");
	}
#undef N_PTRS
}

TEST_BEGIN(test_prof_stats) {
	test_skip_if(!config_prof);
	test_wrapper(0);
	test_wrapper(1);
	test_wrapper(2);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_stats);
}
