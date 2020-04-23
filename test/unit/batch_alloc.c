#include "test/jemalloc_test.h"

#define BATCH_MAX ((1U << 16) + 1024)
static void *ptrs[BATCH_MAX];

#define PAGE_ALIGNED(ptr) (((uintptr_t)ptr & PAGE_MASK) == 0)

static void
verify_batch(void **ptrs, size_t batch, size_t usize, bool zero,
    unsigned nregs) {
	tsd_t *tsd = tsd_fetch();
	assert(tsd != NULL);
	void *ref = NULL; /* Only used when zero is on. */
	if (zero) {
		ref = calloc(1, usize);
		assert_ptr_not_null(ref, "");
	}
	for (size_t i = 0, j = 0; i < batch; ++i, ++j) {
		if (j == nregs) {
			j = 0;
		}
		void *p = ptrs[i];
		expect_zu_eq(isalloc(tsd_tsdn(tsd), p), usize, "");
		if (zero) {
			expect_d_eq(memcmp(p, ref, usize), 0, "");
		}
		if (j == 0) {
			expect_true(PAGE_ALIGNED(p), "");
			continue;
		}
		assert(i > 0);
		void *q = ptrs[i - 1];
		if ((uintptr_t)p > (uintptr_t)q
		    && (size_t)((uintptr_t)p - (uintptr_t)q) == usize) {
			expect_false(prof_sampled(tsd, p)
			    || prof_sampled(tsd, q), "");
		} else if (config_prof && opt_prof) {
			expect_true(PAGE_ALIGNED(p), "");
			expect_true(prof_sampled(tsd, p)
			    || prof_sampled(tsd, q), "");
			j = 0;
		} else {
			expect_not_reached("");
		}
	}
}

static void
release_batch(void **ptrs, size_t batch) {
	for (size_t i = 0; i < batch; ++i) {
		free(ptrs[i]);
	}
}

static void
test_wrapper(size_t size, size_t alignment, bool zero) {
	const size_t usize =
	    (alignment != 0 ? sz_sa2u(size, alignment) : sz_s2u(size));
	szind_t ind = sz_size2index(usize);
	const bin_info_t *bin_info = &bin_infos[ind];
	const unsigned nregs = bin_info->nregs;
	assert(nregs > 0);
	for (size_t i = 0; i < 4; ++i) {
		size_t base = 0;
		if (i == 1) {
			base = nregs;
		} else if (i == 2) {
			base = nregs * 2;
		} else if (i == 3) {
			base = (1 << 16);
		}
		for (int j = -1; j <= 1; ++j) {
			if (base == 0 && j == -1) {
				continue;
			}
			size_t batch = base + (size_t)j;
			assert(batch < BATCH_MAX);
			int flags = 0;
			if (alignment != 0) {
				flags |= MALLOCX_ALIGN(alignment);
			}
			if (zero) {
				flags |= MALLOCX_ZERO;
			}
			size_t filled = batch_alloc(ptrs, batch, size, flags);
			assert_zu_eq(filled, batch, "");
			verify_batch(ptrs, batch, usize, zero, nregs);
			release_batch(ptrs, batch);
		}
	}
}

TEST_BEGIN(test_batch_alloc) {
	test_wrapper(11, 0, false);
}
TEST_END

TEST_BEGIN(test_batch_alloc_zero) {
	test_wrapper(11, 0, true);
}
TEST_END

TEST_BEGIN(test_batch_alloc_aligned) {
	test_wrapper(7, 16, false);
}
TEST_END

TEST_BEGIN(test_batch_alloc_fallback) {
	const size_t size = SC_LARGE_MINCLASS;
	for (size_t batch = 0; batch < 4; ++batch) {
		assert(batch < BATCH_MAX);
		size_t filled = batch_alloc(ptrs, batch, size, 0);
		assert_zu_eq(filled, batch, "");
		release_batch(ptrs, batch);
	}
}
TEST_END

int
main(void) {
	return test(
	    test_batch_alloc,
	    test_batch_alloc_zero,
	    test_batch_alloc_aligned,
	    test_batch_alloc_fallback);
}
