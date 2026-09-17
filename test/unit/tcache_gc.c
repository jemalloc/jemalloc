#include "test/jemalloc_test.h"

extern cache_bin_sz_t tcache_gc_small_nremote_get(
    cache_bin_t *cache_bin, void *addr, uintptr_t *addr_min,
    uintptr_t *addr_max, szind_t szind, size_t nflush);
extern void tcache_gc_small_bin_shuffle(cache_bin_t *cache_bin,
    cache_bin_sz_t nremote, uintptr_t addr_min, uintptr_t addr_max);

static void *
test_cache_bin_init(cache_bin_t *bin, cache_bin_info_t *info,
    cache_bin_sz_t ncached_max) {
	cache_bin_info_init(info, ncached_max);

	size_t size;
	size_t alignment;
	cache_bin_info_compute_alloc(info, 1, &size, &alignment);
	void *mem = mallocx(size, MALLOCX_ALIGN(alignment));
	assert_ptr_not_null(mem, "Unexpected mallocx failure");

	size_t cur_offset = 0;
	cache_bin_preincrement(info, 1, mem, &cur_offset);
	cache_bin_init(bin, info, mem, &cur_offset);
	cache_bin_postincrement(mem, &cur_offset);
	assert_zu_eq(cur_offset, size, "Should use all requested memory");

	return mem;
}

static void
cache_bin_fill_ptrs(cache_bin_t *bin, void **ptrs, cache_bin_sz_t nfill) {
	CACHE_BIN_PTR_ARRAY_DECLARE(arr, nfill);
	cache_bin_init_ptr_array_for_fill(bin, &arr, nfill);
	for (cache_bin_sz_t i = 0; i < nfill; i++) {
		arr.ptr[i] = ptrs[i];
	}
	cache_bin_finish_fill(bin, &arr, nfill);
	expect_zu_eq(nfill, cache_bin_ncached_get_local(bin),
	    "Unexpected fill count");
}

TEST_BEGIN(test_tcache_gc_small_remote_count_and_shuffle) {
	cache_bin_t bin;
	cache_bin_info_t info;
	void *mem = test_cache_bin_init(&bin, &info, 16);

	szind_t szind = 0;
	uintptr_t anchor = ZU(0x40000000);
	size_t slab_size = bin_infos[szind].slab_size;
	void *ptrs[] = {
	    (void *)(anchor + 16),
	    (void *)(anchor + slab_size + 16),
	    (void *)(anchor + 64),
	    (void *)(anchor + TCACHE_GC_NEIGHBOR_LIMIT + PAGE),
	};
	cache_bin_fill_ptrs(&bin, ptrs, 4);

	uintptr_t addr_min;
	uintptr_t addr_max;
	cache_bin_sz_t nremote = tcache_gc_small_nremote_get(&bin,
	    (void *)anchor, &addr_min, &addr_max, szind, 2);
	expect_zu_eq(2, nremote,
	    "Should count pointers outside the local slab");
	expect_zu_eq(anchor, addr_min, "Expected slab-local lower bound");
	expect_zu_eq(anchor + slab_size, addr_max,
	    "Expected slab-local upper bound");

	tcache_gc_small_bin_shuffle(&bin, nremote, addr_min, addr_max);
	expect_ptr_eq(ptrs[0], bin.stack_head[0],
	    "Local pointer order should be preserved");
	expect_ptr_eq(ptrs[2], bin.stack_head[1],
	    "Local pointer order should be preserved");
	for (unsigned i = 2; i < 4; i++) {
		expect_true((uintptr_t)bin.stack_head[i] < addr_min
		        || (uintptr_t)bin.stack_head[i] >= addr_max,
		    "Remote pointers should be moved to the flush side");
	}

	while (cache_bin_ncached_get_local(&bin) > 0) {
		bool success;
		cache_bin_alloc(&bin, &success);
	}
	cache_bin_fill_ptrs(&bin, ptrs, 4);
	nremote = tcache_gc_small_nremote_get(&bin, (void *)anchor,
	    &addr_min, &addr_max, szind, 1);
	expect_zu_eq(1, nremote,
	    "Neighbor filtering should be used when it satisfies nflush");
	expect_zu_eq(anchor - TCACHE_GC_NEIGHBOR_LIMIT, addr_min,
	    "Expected neighbor lower bound");
	expect_zu_eq(anchor + TCACHE_GC_NEIGHBOR_LIMIT, addr_max,
	    "Expected neighbor upper bound");

	free(mem);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_tcache_gc_small_remote_count_and_shuffle);
}
