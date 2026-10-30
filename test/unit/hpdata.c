#include "test/jemalloc_test.h"

#include "test/hpdata.h"

#define HPDATA_ADDR ((void *)(10 * HUGEPAGE))
#define HPDATA_AGE 123

TEST_BEGIN(test_alloc_lfr_one_page) {
	test_skip_if(HUGEPAGE_PAGES < 4);

	/* Commit none, part, or all of a batch into isolated one-page holes. */
	for (size_t committed = 0; committed <= 2; committed++) {
		hpdata_t hpdata;
		hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, false);
		void *addr = hpdata_reserve_alloc(&hpdata, HUGEPAGE);
		hpdata_unreserve(&hpdata, addr, PAGE);
		hpdata_unreserve(&hpdata, (char *)addr + 2 * PAGE, PAGE);
		expect_zu_eq(hpdata_longest_free_range_get(&hpdata), 1, "");

		hpdata_alloc_offset_t offsets[2];
		expect_zu_eq(hpdata_find_alloc_offsets(&hpdata, PAGE, offsets, 2),
		    2, "Both holes should be available");
		for (size_t i = 0; i < committed; i++) {
			hpdata_reserve_alloc_offset(&hpdata, PAGE, &offsets[i]);
		}
		hpdata_post_reserve_alloc_offsets(&hpdata, PAGE, offsets, committed);

		expect_zu_eq(hpdata_longest_free_range_get(&hpdata),
		    committed == 2 ? 0 : 1, "LFR depends on whether any hole remains");
		expect_zu_eq(hpdata_nactive_get(&hpdata),
		    HUGEPAGE_PAGES - 2 + committed, "Committed pages must be counted");
		expect_true(hpdata_consistent(&hpdata),
		    "Cached LFR must agree with the bitmap");
	}
}
TEST_END

TEST_BEGIN(test_reserve_alloc) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, /* is_huge */ false);

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

TEST_BEGIN(test_purge_simple) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, /* is_huge */ false);

	void *alloc = hpdata_reserve_alloc(&hpdata, HUGEPAGE_PAGES / 2 * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Create HUGEPAGE_PAGES / 4 dirty inactive pages at the beginning. */
	hpdata_unreserve(&hpdata, alloc, HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 2, "");

	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t               nranges;
	size_t to_purge = hpdata_purge_begin(&hpdata, &purge_state, &nranges);
	expect_zu_eq(HUGEPAGE_PAGES / 4, to_purge, "");
	expect_zu_eq(1, nranges, "All dirty pages in a single range");

	void  *purge_addr;
	size_t purge_size;
	bool   got_result = hpdata_purge_next(
            &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_false(got_result,
	    "Unexpected additional purge range: "
	    "extent at %p of size %zu",
	    purge_addr, purge_size);

	hpdata_purge_end(&hpdata, &purge_state);
	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 4, "");
}
TEST_END

/*
 * We only test intervening dalloc's not intervening allocs; the latter are
 * disallowed as a purging precondition (because they interfere with purging
 * across a retained extent, saving a purge call).
 */
TEST_BEGIN(test_purge_intervening_dalloc) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, /* is_huge */ false);

	/* Allocate the first 3/4 of the pages. */
	void *alloc = hpdata_reserve_alloc(
	    &hpdata, 3 * HUGEPAGE_PAGES / 4 * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Free the first 1/4 and the third 1/4 of the pages. */
	hpdata_unreserve(&hpdata, alloc, HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 3 * HUGEPAGE_PAGES / 4, "");

	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t               nranges;
	size_t to_purge = hpdata_purge_begin(&hpdata, &purge_state, &nranges);
	expect_zu_eq(HUGEPAGE_PAGES / 2, to_purge, "");
	expect_zu_eq(2, nranges, "First quarter and last half");

	void  *purge_addr;
	size_t purge_size;
	/* First purge. */
	bool got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	/* Deallocate the second 1/4 before the second purge occurs. */
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 1 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	/* Now continue purging. */
	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    purge_addr, "");
	expect_zu_ge(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_false(got_result,
	    "Unexpected additional purge range: "
	    "extent at %p of size %zu",
	    purge_addr, purge_size);

	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 4, "");
}
TEST_END

TEST_BEGIN(test_purge_over_retained) {
	void  *purge_addr;
	size_t purge_size;

	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, /* is_huge */ false);

	/* Allocate the first 3/4 of the pages. */
	void *alloc = hpdata_reserve_alloc(
	    &hpdata, 3 * HUGEPAGE_PAGES / 4 * PAGE);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	/* Free the second quarter. */
	void *second_quarter = (void *)((uintptr_t)alloc
	    + HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata, second_quarter, HUGEPAGE_PAGES / 4 * PAGE);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 3 * HUGEPAGE_PAGES / 4, "");

	/* Purge the second quarter. */
	hpdata_alloc_allowed_set(&hpdata, false);
	hpdata_purge_state_t purge_state;
	size_t               nranges;
	size_t               to_purge_dirty = hpdata_purge_begin(
            &hpdata, &purge_state, &nranges);
	expect_zu_eq(HUGEPAGE_PAGES / 4, to_purge_dirty, "");
	expect_zu_eq(1, nranges, "Second quarter only");

	bool got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(second_quarter, purge_addr, "");
	expect_zu_eq(HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_false(got_result,
	    "Unexpected additional purge range: "
	    "extent at %p of size %zu",
	    purge_addr, purge_size);
	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), HUGEPAGE_PAGES / 2, "");

	/* Free the first and third quarter. */
	hpdata_unreserve(&hpdata, HPDATA_ADDR, HUGEPAGE_PAGES / 4 * PAGE);
	hpdata_unreserve(&hpdata,
	    (void *)((uintptr_t)alloc + 2 * HUGEPAGE_PAGES / 4 * PAGE),
	    HUGEPAGE_PAGES / 4 * PAGE);

	/*
	 * Purge again.  The second quarter is retained, so we can safely
	 * re-purge it.  We expect a single purge of 3/4 of the hugepage,
	 * purging half its pages.
	 */
	to_purge_dirty = hpdata_purge_begin(&hpdata, &purge_state, &nranges);
	expect_zu_eq(HUGEPAGE_PAGES / 2, to_purge_dirty, "");
	expect_zu_eq(1, nranges, "Single range expected");

	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_true(got_result, "");
	expect_ptr_eq(HPDATA_ADDR, purge_addr, "");
	expect_zu_eq(3 * HUGEPAGE_PAGES / 4 * PAGE, purge_size, "");

	got_result = hpdata_purge_next(
	    &hpdata, &purge_state, &purge_addr, &purge_size);
	expect_false(got_result,
	    "Unexpected additional purge range: "
	    "extent at %p of size %zu",
	    purge_addr, purge_size);
	hpdata_purge_end(&hpdata, &purge_state);

	expect_zu_eq(hpdata_ntouched_get(&hpdata), 0, "");
}
TEST_END

TEST_BEGIN(test_hugify) {
	hpdata_t hpdata;
	hpdata_init(&hpdata, HPDATA_ADDR, HPDATA_AGE, /* is_huge */ false);

	void *alloc = hpdata_reserve_alloc(&hpdata, HUGEPAGE / 2);
	expect_ptr_eq(alloc, HPDATA_ADDR, "");

	expect_zu_eq(HUGEPAGE_PAGES / 2, hpdata_ntouched_get(&hpdata), "");

	hpdata_hugify(&hpdata);

	/* Hugeifying should have increased the dirty page count. */
	expect_zu_eq(HUGEPAGE_PAGES, hpdata_ntouched_get(&hpdata), "");
}
TEST_END

TEST_BEGIN(test_layout) {
	/* we don't care about the layout in debug builds */
	test_skip_if(config_debug);

	/* check the layout for 64-bit builds with 4K pages and 2M hugepages */
	test_skip_if(LG_SIZEOF_PTR != 3);
	test_skip_if(PAGE != 4 * 1024);
	test_skip_if(HUGEPAGE != 2 * 1024 * 1024);

	/*
	 * hpdata is allocated at the cache line granularity (e.g., 64 bytes).
	 * So, if it's a bit over 256, the allocation will be rounded to 320.
	 * hpdata is a very small percentage of rss, so this
	 * doesn't really matter. But let's try to keep it <= 256.
	 */
	expect_zu_le(sizeof(hpdata_t), 4 * CACHELINE,
	    "we're expecting hpdata_t to be <= 4 x cache line");

	/*
	 * Let's try to keep the two bitmaps cache line-aligned,
	 * which currently also means that each bitmap resides
	 * on a single cache line.
	 */
	expect_zu_eq(offsetof(hpdata_t, active_pages) % CACHELINE, 0,
	    "we're expecting active_pages to be cache line-aligned");
	expect_zu_eq(offsetof(hpdata_t, touched_pages) % CACHELINE, 0,
	    "we're expecting touched_pages to be cache line-aligned");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_alloc_lfr_one_page, test_reserve_alloc,
	    test_purge_simple,
	    test_purge_intervening_dalloc, test_purge_over_retained,
	    test_hugify, test_layout);
}
