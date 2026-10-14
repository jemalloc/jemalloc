#include "test/jemalloc_test.h"

TEST_BEGIN(test_free_sized) {
	void *p = mallocx(42, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	free_sized(p, 42);
}
TEST_END

TEST_BEGIN(test_free_aligned_sized) {
	size_t alignment = 0x100;
	void  *p = mallocx(42, MALLOCX_ALIGN(alignment));
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	free_aligned_sized(p, alignment, 42);
}
TEST_END

TEST_BEGIN(test_free_aligned_sized_small_alignment) {
	const size_t size = 8;
	const size_t alignment = 16;
	void *p = mallocx(size, MALLOCX_ALIGN(alignment));
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_zu_eq((uintptr_t)p & (alignment - 1), 0,
	    "Allocation does not satisfy the requested alignment");

	/* Pass the original requested size and alignment. */
	free_aligned_sized(p, alignment, size);

	/* Flush now so an incorrect cache-bin insertion cannot remain hidden. */
	bool opt_tcache;
	size_t sz = sizeof(opt_tcache);
	expect_d_eq(mallctl("opt.tcache", (void *)&opt_tcache, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    opt_tcache ? 0 : EFAULT, "Unexpected tcache flush failure");
}
TEST_END

TEST_BEGIN(test_free_sized_null) {
	/*
	 * C23 specifies that free_sized(NULL, size) and
	 * free_aligned_sized(NULL, alignment, size) do nothing, just as
	 * free(NULL) does.  The size argument is ignored for a NULL pointer.
	 */
	free_sized(NULL, 0);
	free_sized(NULL, 42);
	free_aligned_sized(NULL, 0x100, 0);
	free_aligned_sized(NULL, 0x100, 42);
}
TEST_END

int
main(void) {
	return test(
	    test_free_sized,
	    test_free_aligned_sized,
	    test_free_aligned_sized_small_alignment,
	    test_free_sized_null);
}
