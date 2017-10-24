#include "test/jemalloc_test.h"

#include "jemalloc/internal/sized_region.h"

#define ALLOCS_PER_SC 10

TEST_BEGIN(test_lookup) {
	sized_region_t region;
	sized_region_init(&region);
	void *allocs[SIZED_REGION_NUM_SCS][ALLOCS_PER_SC];
	for (unsigned sc = 0; sc < SIZED_REGION_NUM_SCS; sc++) {
		for (int alloc = 0; alloc < ALLOCS_PER_SC; alloc++) {
			bool zero = false;
			bool commit = false;

			size_t size = PAGE * (alloc + 1);
			void *ptr = sized_region_alloc(TSDN_NULL, &region, size,
			    sc, true, &zero, &commit);
			allocs[sc][alloc] = ptr;
			assert_true(zero, "Got non-zero alloc.");
			assert_ptr_not_null(ptr, "Unexpected null alloc.");
		}
	}

	for (unsigned sc = 0; sc < SIZED_REGION_NUM_SCS; sc++) {
		for (int alloc = 0; alloc < ALLOCS_PER_SC; alloc++) {
			alloc_ctx_t alloc_ctx = {0, false};
			char *begin = (char *)allocs[sc][alloc];
			char *end = begin + (alloc + 1) * PAGE;
			for (char *p = begin; p < end; p++) {
				bool success = sized_region_lookup(
				    &region, (void *)p, &alloc_ctx);
				assert_true(success, "Lookup failure");
				assert_d_eq(sc, (int)alloc_ctx.szind,
				    "Lookup found incorrect size class.");
				assert_true(alloc_ctx.slab,
				    "Lookup found non-slab alloc.");
			}
		}
	}

	sized_region_destroy(&region);

}
TEST_END

TEST_BEGIN(test_overflow) {
	sized_region_t region;
	sized_region_init(&region);
	/* Pick an arbitrary size class. */
	unsigned sc = 7;

	/*
	 * We include a "+ 1" on the loop boundary to make sure we go *past* the
	 * end of the size class range.
	 */
	size_t sc_size = (ZU(1) << opt_lg_sized_region_size);
	for (unsigned i = 0; i < sc_size / (100 * PAGE) + 1;
	    i++) {
		bool zero = false;
		bool commit = false;
		sized_region_alloc(TSDN_NULL, &region, 100 * PAGE, sc, true,
		    &zero, &commit);
	}
	bool zero = false;
	bool commit = false;
	void *ptr = sized_region_alloc(TSDN_NULL, &region, 100 * PAGE, sc, true,
	    &zero, &commit);
	assert_ptr_null(ptr, "Kept allocating even after space exhausted.");

	sized_region_destroy(&region);
}
TEST_END

TEST_BEGIN(test_zero_init) {
	/*
	 * An uninitialized sized_region_t should return NULL for
	 * allocations occurring before initialization.
	 */
	sized_region_t region;
	memset(&region, 0, sizeof(region));
	bool zero = false;
	bool commit = false;
	void *ptr = sized_region_alloc(TSDN_NULL, &region, PAGE, 0, true, &zero,
	    &commit);
	assert_ptr_null(ptr, "Zero-initialized region should not allocate.");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_lookup,
	    test_overflow,
	    test_zero_init);
}
