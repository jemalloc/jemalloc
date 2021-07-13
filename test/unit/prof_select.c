#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_data.h"

static void
check(size_t size, int flags, bool sample) {
	tsd_t *tsd = tsd_fetch();
	prof_cnt_t cnt_orig, cnt;

	prof_cnt_all(&cnt_orig);

	void *p = mallocx(size, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	prof_info_t prof_info;
	prof_info_get(tsd, p, NULL, &prof_info);
	expect_b_eq(prof_info.alloc_tctx != (prof_tctx_t *)(uintptr_t)1U,
	    sample, "");
	prof_cnt_all(&cnt);
	expect_u64_eq(cnt_orig.curobjs + (sample ? 1 : 0), cnt.curobjs, "");

	dallocx(p, flags);
	prof_cnt_all(&cnt);
	expect_u64_eq(cnt_orig.curobjs, cnt.curobjs, "");
}

TEST_BEGIN(test_prof_select) {
	test_skip_if(!config_prof);

	check(8, 0, false);
	check(8, MALLOCX_ALIGN(32), true);
	check(12, 0, false);
	check(12, MALLOCX_ALIGN(32), true);
	check(17, 0, true);
	check(17, MALLOCX_ALIGN(64), false);
	check(20, 0, true);
	check(20, MALLOCX_ALIGN(64), false);
	check(21, 0, false);
	check(32, 0, false);
	check(38, 0, false);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_select);
}
