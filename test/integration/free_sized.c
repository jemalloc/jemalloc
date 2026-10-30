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

TEST_BEGIN(test_free_aligned_sized_sweep) {
	bool opt_tcache;
	size_t sz = sizeof(opt_tcache);
	expect_d_eq(mallctl("opt.tcache", (void *)&opt_tcache, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	size_t alignments[] = {sizeof(void *), 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
	for (size_t a = 0; a < sizeof(alignments) / sizeof(alignments[0]); a++) {
		size_t alignment = alignments[a];
		size_t sizes[] = {
			1,
			alignment / 2,
			alignment > 1 ? alignment - 1 : 1,
			alignment,
			alignment + 1,
			alignment * 2,
			8, 15, 16, 17, 31, 32, 48, 64, 80, 100, 128
		};
		for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			size_t size = sizes[s];
			if (size == 0) {
				continue;
			}

			/* Test mallocx + free_aligned_sized. */
			void *p = mallocx(size, MALLOCX_ALIGN(alignment));
			assert_ptr_not_null(p, "Unexpected mallocx() failure");
			expect_zu_eq((uintptr_t)p & (alignment - 1), 0,
			    "Allocation does not satisfy the requested alignment");
			free_aligned_sized(p, alignment, size);
			expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
			    opt_tcache ? 0 : EFAULT, "Unexpected tcache flush failure");

			/* Test posix_memalign + free_aligned_sized. */
			if (alignment >= sizeof(void *)) {
				void *p_memalign = NULL;
				int err = posix_memalign(&p_memalign, alignment, size);
				expect_d_eq(err, 0, "Unexpected posix_memalign() failure");
				expect_zu_eq((uintptr_t)p_memalign & (alignment - 1), 0,
				    "Allocation does not satisfy the requested alignment");
				free_aligned_sized(p_memalign, alignment, size);
				expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
				    opt_tcache ? 0 : EFAULT, "Unexpected tcache flush failure");
			}

			/* Test aligned_alloc + free_aligned_sized with aligned size. */
			size_t rounded_size = (size + alignment - 1) & ~(alignment - 1);
			void *p_aligned = aligned_alloc(alignment, rounded_size);
			assert_ptr_not_null(p_aligned, "Unexpected aligned_alloc() failure");
			expect_zu_eq((uintptr_t)p_aligned & (alignment - 1), 0,
			    "Allocation does not satisfy the requested alignment");
			free_aligned_sized(p_aligned, alignment, rounded_size);
			expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
			    opt_tcache ? 0 : EFAULT, "Unexpected tcache flush failure");
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
	    test_free_aligned_sized_sweep,
	    test_free_sized_null);
}
