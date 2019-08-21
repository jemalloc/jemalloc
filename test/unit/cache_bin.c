#include "test/jemalloc_test.h"

cache_bin_t test_bin;

TEST_BEGIN(test_cache_bin) {
	cache_bin_t *bin = &test_bin;
	cassert(PAGE > TCACHE_NSLOTS_SMALL_MAX * sizeof(void *));
	/* Page aligned to make sure lowbits not overflowable. */
	void **stack = mallocx(PAGE, MALLOCX_TCACHE_NONE | MALLOCX_ALIGN(PAGE));

	assert_ptr_not_null(stack, "Unexpected mallocx failure");
	/* Initialize to empty; bin 0. */
	cache_bin_sz_t ncached_max = cache_bin_ncached_max_get(0);
	void **empty_position = stack + ncached_max;
	bin->cur_ptr.ptr = empty_position;
	bin->low_water_position = bin->cur_ptr.lowbits;
	bin->full_position = (uint32_t)(uintptr_t)stack;
	assert_ptr_eq(cache_bin_empty_position_get(bin, 0), empty_position,
	    "Incorrect empty position");
	/* Not using assert_zu etc on cache_bin_sz_t since it may change. */
	assert_true(cache_bin_ncached_get(bin, 0) == 0, "Incorrect cache size");

	bool success;
	void *ret = cache_bin_alloc_easy(bin, &success, 0);
	assert_false(success, "Empty cache bin should not alloc");
	assert_true(cache_bin_low_water_get(bin, 0) == 0,
	    "Incorrect low water mark");

	cache_bin_ncached_set(bin, 0, 0);
	assert_ptr_eq(bin->cur_ptr.ptr, empty_position, "Bin should be empty");
	for (cache_bin_sz_t i = 1; i < ncached_max + 1; i++) {
		success = cache_bin_dalloc_easy(bin, (void *)(uintptr_t)i);
		assert_true(success && cache_bin_ncached_get(bin, 0) == i,
		    "Bin dalloc failure");
	}
	success = cache_bin_dalloc_easy(bin, (void *)1);
	assert_false(success, "Bin should be full");
	assert_ptr_eq(bin->cur_ptr.ptr, stack, "Incorrect bin cur_ptr");

	cache_bin_ncached_set(bin, 0, ncached_max);
	assert_ptr_eq(bin->cur_ptr.ptr, stack, "cur_ptr should not change");
	/* Emulate low water after refill. */
	bin->low_water_position = bin->full_position;
	for (cache_bin_sz_t i = ncached_max; i > 0; i--) {
		ret = cache_bin_alloc_easy(bin, &success, 0);
		cache_bin_sz_t ncached = cache_bin_ncached_get(bin, 0);
		assert_true(success && ncached == i - 1,
		    "Cache bin alloc failure");
		assert_ptr_eq(ret, (void *)(uintptr_t)i, "Bin alloc failure");
		assert_true(cache_bin_low_water_get(bin, 0) == ncached,
		    "Incorrect low water mark");
	}

	ret = cache_bin_alloc_easy(bin, &success, 0);
	assert_false(success, "Empty cache bin should not alloc.");
	assert_ptr_eq(bin->cur_ptr.ptr, stack + ncached_max,
	    "Bin should be empty");
}
TEST_END

int
main(void) {
	return test(test_cache_bin);
}
