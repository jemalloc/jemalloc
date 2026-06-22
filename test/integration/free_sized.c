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
	    test_free_sized_null);
}
