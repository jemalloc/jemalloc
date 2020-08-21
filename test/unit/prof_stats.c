#include "test/jemalloc_test.h"

static void
test_wrapper(szind_t ind) {
#define N_PTRS 3
#define MALLCTL_STR_LEN 64
	assert(opt_prof && opt_prof_stats);

	char mallctl_live_str[MALLCTL_STR_LEN];
	char mallctl_accum_str[MALLCTL_STR_LEN];
	if (ind < SC_NBINS) {
		malloc_snprintf(mallctl_live_str, MALLCTL_STR_LEN,
		    "prof.stats.bins.%u.live", (unsigned)ind);
		malloc_snprintf(mallctl_accum_str, MALLCTL_STR_LEN,
		    "prof.stats.bins.%u.accum", (unsigned)ind);
	} else {
		malloc_snprintf(mallctl_live_str, MALLCTL_STR_LEN,
		    "prof.stats.lextents.%u.live", (unsigned)(ind - SC_NBINS));
		malloc_snprintf(mallctl_accum_str, MALLCTL_STR_LEN,
		    "prof.stats.lextents.%u.accum", (unsigned)(ind - SC_NBINS));
	}

	size_t stats_len = 2 * sizeof(uint64_t);

	uint64_t live_stats_orig[2];
	assert_d_eq(mallctl(mallctl_live_str, &live_stats_orig, &stats_len,
	    NULL, 0), 0, "");
	uint64_t accum_stats_orig[2];
	assert_d_eq(mallctl(mallctl_accum_str, &accum_stats_orig, &stats_len,
	    NULL, 0), 0, "");

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
		uint64_t live_stats[2];
		assert_d_eq(mallctl(mallctl_live_str, &live_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(live_stats[0] - live_stats_orig[0],
		    live_req_sum, "");
		expect_u64_eq(live_stats[1] - live_stats_orig[1],
		    live_count, "");
		uint64_t accum_stats[2];
		assert_d_eq(mallctl(mallctl_accum_str, &accum_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(accum_stats[0] - accum_stats_orig[0],
		    accum_req_sum, "");
		expect_u64_eq(accum_stats[1] - accum_stats_orig[1],
		    accum_count, "");
	}

	for (size_t i = 0, sz = sz_index2size(ind) - N_PTRS; i < N_PTRS;
	    ++i, ++sz) {
		free(ptrs[i]);
		live_req_sum -= sz;
		live_count--;
		uint64_t live_stats[2];
		assert_d_eq(mallctl(mallctl_live_str, &live_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(live_stats[0] - live_stats_orig[0],
		    live_req_sum, "");
		expect_u64_eq(live_stats[1] - live_stats_orig[1],
		    live_count, "");
		uint64_t accum_stats[2];
		assert_d_eq(mallctl(mallctl_accum_str, &accum_stats, &stats_len,
		    NULL, 0), 0, "");
		expect_u64_eq(accum_stats[0] - accum_stats_orig[0],
		    accum_req_sum, "");
		expect_u64_eq(accum_stats[1] - accum_stats_orig[1],
		    accum_count, "");
	}
#undef MALLCTL_STR_LEN
#undef N_PTRS
}

TEST_BEGIN(test_prof_stats) {
	test_skip_if(!config_prof);
	test_wrapper(0);
	test_wrapper(1);
	test_wrapper(2);
	test_wrapper(SC_NBINS);
	test_wrapper(SC_NBINS + 1);
	test_wrapper(SC_NBINS + 2);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_stats);
}
