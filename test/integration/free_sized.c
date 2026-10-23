#include "test/jemalloc_test.h"

TEST_BEGIN(test_free_sized) {
	void *p = mallocx(42, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	free_sized(p, 42);
}
TEST_END

TEST_BEGIN(test_free_aligned_sized) {
	const size_t alignments[] = {1, 8, 16, 64, 256, 4096, 65536};
	const size_t sizes[] = {1, 42, 64, 4096, 65536};

	for (unsigned i = 0; i < sizeof(alignments) / sizeof(alignments[0]);
	     i++) {
		for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
			size_t alignment = alignments[i];
			size_t size = sizes[j];
			void *p = mallocx(size, MALLOCX_ALIGN(alignment));
			assert_ptr_not_null(p,
			    "mallocx() failed for alignment=%zu, size=%zu",
			    alignment, size);
			expect_zu_eq((uintptr_t)p & (alignment - 1), 0,
			    "Unexpected pointer alignment for alignment=%zu, "
			    "size=%zu", alignment, size);
			memset(p, 0xa5, size);
			free_aligned_sized(p, alignment, size);
		}
	}
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
