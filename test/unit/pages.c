#include "test/jemalloc_test.h"

TEST_BEGIN(test_pages_huge) {
	size_t alloc_size;
	bool   commit;
	void  *pages, *hugepage;

	alloc_size = HUGEPAGE * 2 - PAGE;
	commit = true;
	pages = pages_map(NULL, alloc_size, PAGE, &commit);
	expect_ptr_not_null(pages, "Unexpected pages_map() error");

	if (init_system_thp_mode == system_thp_mode_madvise) {
		hugepage = (void *)(ALIGNMENT_CEILING(
		    (uintptr_t)pages, HUGEPAGE));
		expect_b_ne(pages_huge(hugepage, HUGEPAGE), have_madvise_huge,
		    "Unexpected pages_huge() result");
		expect_false(pages_nohuge(hugepage, HUGEPAGE),
		    "Unexpected pages_nohuge() result");
	}

	pages_unmap(pages, alloc_size);
}
TEST_END

TEST_BEGIN(test_pages_page_id_errno) {
#ifndef JEMALLOC_PAGEID
	test_skip_if(true);
#else
	bool  commit = true;
	void *pages = pages_map(NULL, PAGE * 2, PAGE, &commit);
	expect_ptr_not_null(pages, "Unexpected pages_map() error");

	/*
	 * A misaligned address makes the naming prctl() fail with EINVAL on
	 * every Linux kernel, as all calls do on kernels without
	 * CONFIG_ANON_VMA_NAME.
	 */
	set_errno(EAGAIN);
	os_page_id((void *)((uintptr_t)pages + 1), PAGE, "jemalloc_test");
	expect_d_eq(get_errno(), EAGAIN, "os_page_id() changed errno");

	pages_unmap(pages, PAGE * 2);
#endif
}
TEST_END

int
main(void) {
	return test(test_pages_huge, test_pages_page_id_errno);
}
