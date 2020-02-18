#include "test/jemalloc_test.h"

TEST_BEGIN(test_prof_realloc) {
	tsd_t *tsd;
	int flags;
	void *p, *q;
	prof_info_t prof_info_p, prof_info_q;
	uint64_t curobjs_0, curobjs_1, curobjs_2, curobjs_3;

	test_skip_if(!config_prof);

	tsd = tsd_fetch();
	flags = MALLOCX_TCACHE_NONE;

	prof_cnt_all(&curobjs_0, NULL, NULL, NULL);
	p = mallocx(1024, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	prof_info_get(tsd, p, NULL, &prof_info_p);
	expect_ptr_ne(prof_info_p.alloc_tctx, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&curobjs_1, NULL, NULL, NULL);
	expect_u64_eq(curobjs_0 + 1, curobjs_1,
	    "Allocation should have increased sample size");

	q = rallocx(p, 2048, flags);
	expect_ptr_ne(p, q, "Expected move");
	expect_ptr_not_null(p, "Unexpected rmallocx() failure");
	prof_info_get(tsd, q, NULL, &prof_info_q);
	expect_ptr_ne(prof_info_q.alloc_tctx, (prof_tctx_t *)(uintptr_t)1U,
	    "Expected valid tctx");
	prof_cnt_all(&curobjs_2, NULL, NULL, NULL);
	expect_u64_eq(curobjs_1, curobjs_2,
	    "Reallocation should not have changed sample size");

	dallocx(q, flags);
	prof_cnt_all(&curobjs_3, NULL, NULL, NULL);
	expect_u64_eq(curobjs_0, curobjs_3,
	    "Sample size should have returned to base level");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_prof_realloc);
}
