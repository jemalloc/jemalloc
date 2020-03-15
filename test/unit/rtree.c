#include "test/jemalloc_test.h"

#include "jemalloc/internal/rtree.h"

#define INVALID_ARENA_IND ((1U << MALLOCX_ARENA_BITS) - 1)

/* Potentially too large to safely place on the stack. */
rtree_t test_rtree;

TEST_BEGIN(test_rtree_read_empty) {
	tsdn_t *tsdn;

	tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");
	expect_ptr_null(rtree_edata_read(tsdn, rtree, &rtree_ctx, PAGE,
	    false), "rtree_edata_read() should return NULL for empty tree");

	base_delete(tsdn, base);
}
TEST_END

#undef NTHREADS
#undef NITERS
#undef SEED

TEST_BEGIN(test_rtree_extrema) {
	edata_t edata_a, edata_b;
	edata_init(&edata_a, INVALID_ARENA_IND, NULL, SC_LARGE_MINCLASS,
	    false, sz_size2index(SC_LARGE_MINCLASS), 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);
	edata_init(&edata_b, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);

	tsdn_t *tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, &edata_a,
	    edata_szind_get(&edata_a), edata_slab_get(&edata_a)),
	    "Unexpected rtree_write() failure");
	rtree_szind_slab_update(tsdn, rtree, &rtree_ctx, PAGE,
	    edata_szind_get(&edata_a), edata_slab_get(&edata_a));
	expect_ptr_eq(rtree_edata_read(tsdn, rtree, &rtree_ctx, PAGE, true),
	    &edata_a,
	    "rtree_edata_read() should return previously set value");

	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, ~((uintptr_t)0),
	    &edata_b, edata_szind_get_maybe_invalid(&edata_b),
	    edata_slab_get(&edata_b)), "Unexpected rtree_write() failure");
	expect_ptr_eq(rtree_edata_read(tsdn, rtree, &rtree_ctx,
	    ~((uintptr_t)0), true), &edata_b,
	    "rtree_edata_read() should return previously set value");

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_rtree_bits) {
	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	uintptr_t keys[] = {PAGE, PAGE + 1,
	    PAGE + (((uintptr_t)1) << LG_PAGE) - 1};

	edata_t edata;
	edata_init(&edata, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < sizeof(keys)/sizeof(uintptr_t); i++) {
		expect_false(rtree_write(tsdn, rtree, &rtree_ctx, keys[i],
		    &edata, SC_NSIZES, false),
		    "Unexpected rtree_write() failure");
		for (unsigned j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			expect_ptr_eq(rtree_edata_read(tsdn, rtree, &rtree_ctx,
			    keys[j], true), &edata,
			    "rtree_edata_read() should return previously set "
			    "value and ignore insignificant key bits; i=%u, "
			    "j=%u, set key=%#"FMTxPTR", get key=%#"FMTxPTR, i,
			    j, keys[i], keys[j]);
		}
		expect_ptr_null(rtree_edata_read(tsdn, rtree, &rtree_ctx,
		    (((uintptr_t)2) << LG_PAGE), false),
		    "Only leftmost rtree leaf should be set; i=%u", i);
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
	}

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_rtree_random) {
#define NSET 16
#define SEED 42
	sfmt_t *sfmt = init_gen_rand(SEED);
	tsdn_t *tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	uintptr_t keys[NSET];
	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);

	edata_t edata;
	edata_init(&edata, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);

	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < NSET; i++) {
		keys[i] = (uintptr_t)gen_rand64(sfmt);
		rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree,
		    &rtree_ctx, keys[i], false, true);
		expect_ptr_not_null(elm,
		    "Unexpected rtree_leaf_elm_lookup() failure");
		rtree_contents_t contents;
		contents.edata = &edata;
		contents.metadata.szind = SC_NSIZES;
		contents.metadata.slab = false;
		rtree_leaf_elm_write(tsdn, rtree, elm, contents);
		expect_ptr_eq(rtree_edata_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true), &edata,
		    "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_eq(rtree_edata_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true), &edata,
		    "rtree_edata_read() should return previously set value, "
		    "i=%u", i);
	}

	for (unsigned i = 0; i < NSET; i++) {
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
		expect_ptr_null(rtree_edata_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true),
		   "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_null(rtree_edata_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true),
		    "rtree_edata_read() should return previously set value");
	}

	base_delete(tsdn, base);
	fini_gen_rand(sfmt);
#undef NSET
#undef SEED
}
TEST_END

int
main(void) {
	return test(
	    test_rtree_read_empty,
	    test_rtree_extrema,
	    test_rtree_bits,
	    test_rtree_random);
}
