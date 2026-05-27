#include "test/jemalloc_test.h"

extern cache_bin_sz_t tcache_gc_small_nremote_get_test(
    cache_bin_t *cache_bin, void *addr, uintptr_t *addr_min,
    uintptr_t *addr_max, szind_t szind, size_t nflush);
extern void tcache_gc_small_bin_shuffle_test(cache_bin_t *cache_bin,
    cache_bin_sz_t nremote, uintptr_t addr_min, uintptr_t addr_max);
extern uint8_t tcache_nfill_small_lg_div_get_test(
    tcache_slow_t *tcache_slow, szind_t szind);
extern void tcache_nfill_small_burst_prepare_test(
    tcache_slow_t *tcache_slow, szind_t szind);
extern void tcache_nfill_small_burst_reset_test(
    tcache_slow_t *tcache_slow, szind_t szind);
extern void tcache_nfill_small_gc_update_test(
    tcache_slow_t *tcache_slow, szind_t szind, cache_bin_sz_t limit);
extern uint8_t tcache_gc_item_delay_compute_test(szind_t szind);

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
	cache_bin_sz_t nremote = tcache_gc_small_nremote_get_test(&bin,
	    (void *)anchor, &addr_min, &addr_max, szind, 2);
	expect_zu_eq(2, nremote,
	    "Should count pointers outside the local slab");
	expect_zu_eq(anchor, addr_min, "Expected slab-local lower bound");
	expect_zu_eq(anchor + slab_size, addr_max,
	    "Expected slab-local upper bound");

	tcache_gc_small_bin_shuffle_test(&bin, nremote, addr_min, addr_max);
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
	nremote = tcache_gc_small_nremote_get_test(&bin, (void *)anchor,
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

TEST_BEGIN(test_tcache_gc_fill_control_and_delay) {
	tcache_slow_t tcache_slow;
	memset(&tcache_slow, 0, sizeof(tcache_slow));

	szind_t szind = 0;
	cache_bin_fill_ctl_t *ctl =
	    &tcache_slow.bin_fill_ctl_do_not_access_directly[szind];
	ctl->base = 3;
	ctl->offset = 0;

	bool old_experimental_tcache_gc = opt_experimental_tcache_gc;
	size_t old_tcache_gc_delay_bytes = opt_tcache_gc_delay_bytes;

	opt_experimental_tcache_gc = true;
	expect_u_eq(3, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Unexpected initial fill divisor");
	tcache_nfill_small_burst_prepare_test(&tcache_slow, szind);
	expect_u_eq(2, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Burst load should increase fill count");
	tcache_nfill_small_burst_prepare_test(&tcache_slow, szind);
	expect_u_eq(1, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Burst load should cap at divisor 1");
	tcache_nfill_small_burst_prepare_test(&tcache_slow, szind);
	expect_u_eq(1, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Burst offset should not reach base");

	tcache_nfill_small_burst_reset_test(&tcache_slow, szind);
	expect_u_eq(3, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Burst reset should clear offset");

	tcache_nfill_small_gc_update_test(&tcache_slow, szind, 0);
	expect_u_eq(2, ctl->base,
	    "Refill during a GC period should increase future fill count");
	expect_u_eq(0, ctl->offset, "GC update should reset burst offset");

	tcache_nfill_small_gc_update_test(&tcache_slow, szind, 64);
	expect_u_eq(3, ctl->base,
	    "Low-water pressure should reduce future fill count");

	ctl->offset = 2;
	opt_experimental_tcache_gc = false;
	expect_u_eq(3, tcache_nfill_small_lg_div_get_test(
	    &tcache_slow, szind), "Legacy GC should ignore burst offset");

	size_t sz = sz_index2size(szind);
	opt_tcache_gc_delay_bytes = 3 * sz;
	expect_u_eq(3, tcache_gc_item_delay_compute_test(szind),
	    "Delay should convert bytes to items");
	opt_tcache_gc_delay_bytes = SIZE_T_MAX;
	expect_u_eq(UINT8_MAX, tcache_gc_item_delay_compute_test(szind),
	    "Delay should saturate at uint8 max");

	opt_experimental_tcache_gc = old_experimental_tcache_gc;
	opt_tcache_gc_delay_bytes = old_tcache_gc_delay_bytes;
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_tcache_gc_small_remote_count_and_shuffle,
	    test_tcache_gc_fill_control_and_delay);
}
