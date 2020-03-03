#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_data.h"

static void
verify_prof_cnt_all(prof_cnt_t *cnt_all) {
	/*
	 * When lg_prof_sample is 0, the prof sample wait time is always set to
	 * be 1, and thus the surplus should always be one less than the usize
	 * for every allocation.  Therefore, the usize sum is always equal to
	 * the number of allocations plus the surplus sum.
	 */
	expect_u64_eq(cnt_all->curobjs + cnt_all->cursurplus,
	    cnt_all->curbytes, "");
	expect_u64_eq(cnt_all->accumobjs + cnt_all->accumsurplus,
	    cnt_all->accumbytes, "");
}

TEST_BEGIN(test_prof_surplus) {
	test_skip_if(!config_prof);

	void *p, *q;
	prof_cnt_t cnt;

	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);

	p = malloc(1024);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
	free(p);
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);

	p = malloc(1024);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	q = malloc(2048);
	assert_ptr_not_null(q, "Unexpected malloc() failure");
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
	free(q);
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
	free(p);
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);

	p = malloc(1024);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	q = realloc(p, 2048);
	assert_ptr_ne(p, q, "Expected move");
	assert_ptr_not_null(q, "Unexpected realloc() failure");
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
	free(q);
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);

	p = malloc(1024);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	size_t s = xallocx(p, 2048, 0, 0);
	assert_zu_eq(s, 1024, "Expected stay");
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
	free(p);
	prof_cnt_all(&cnt);
	verify_prof_cnt_all(&cnt);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_surplus);
}
