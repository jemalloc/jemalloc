#include "test/jemalloc_test.h"

#define HPDATA_ADDR ((void *)(10 * HUGEPAGE))
#define HPDATA_AGE 123

TEST_BEGIN(test_reserve_alloc) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE);

	/* Allocating a page at a time, we should do first fit. */
	for (size_t i = 0; i < HUGEPAGE_PAGES; i++) {
		expect_true(hpdata_consistent(&hpdata), "");
		expect_zu_eq(HUGEPAGE_PAGES - i,
		    hpdata_longest_free_range_get(&hpdata), "");
		void *alloc = hpdata_reserve_alloc(&hpdata, PAGE);
		expect_ptr_eq((char *)HPDATA_ADDR + i * PAGE, alloc, "");
		expect_true(hpdata_consistent(&hpdata), "");
	}
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(0, hpdata_longest_free_range_get(&hpdata), "");

	/*
	 * Build up a bigger free-range, 2 pages at a time, until we've got 6
	 * adjacent free pages total.  Pages 8-13 should be unreserved after
	 * this.
	 */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 10 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(2, hpdata_longest_free_range_get(&hpdata), "");

	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 12 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(4, hpdata_longest_free_range_get(&hpdata), "");

	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 8 * PAGE, 2 * PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(6, hpdata_longest_free_range_get(&hpdata), "");

	/*
	 * Leave page 14 reserved, but free page 15 (this test the case where
	 * unreserving combines two ranges).
	 */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 15 * PAGE, PAGE);
	/*
	 * Longest free range shouldn't change; we've got a free range of size
	 * 6, then a reserved page, then another free range.
	 */
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(6, hpdata_longest_free_range_get(&hpdata), "");

	/* After freeing page 14, the two ranges get combined. */
	hpdata_unreserve(&hpdata, (char *)HPDATA_ADDR + 14 * PAGE, PAGE);
	expect_true(hpdata_consistent(&hpdata), "");
	expect_zu_eq(8, hpdata_longest_free_range_get(&hpdata), "");
}
TEST_END

int main(void) {
	return test_no_reentrancy(
	    test_reserve_alloc);
}
