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
	rtree_contents_t contents;
	expect_true(rtree_read_independent(tsdn, rtree, &rtree_ctx, PAGE,
	    &contents), "rtree_read_independent() should fail on empty rtree.");

	base_delete(tsdn, base);
}
TEST_END

#undef NTHREADS
#undef NITERS
#undef SEED

TEST_BEGIN(test_rtree_extrema) {
	edata_t edata_a = {0}, edata_b = {0};
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

	rtree_contents_t contents_a;
	contents_a.edata = &edata_a;
	contents_a.metadata.szind = edata_szind_get(&edata_a);
	contents_a.metadata.slab = edata_slab_get(&edata_a);
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, contents_a),
	    "Unexpected rtree_write() failure");
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, contents_a),
	    "Unexpected rtree_write() failure");
	rtree_contents_t read_contents_a = rtree_read(tsdn, rtree, &rtree_ctx,
	    PAGE);
	expect_true(contents_a.edata == read_contents_a.edata
	    && contents_a.metadata.szind == read_contents_a.metadata.szind
	    && contents_a.metadata.slab == read_contents_a.metadata.slab,
	    "rtree_read() should return previously set value");

	rtree_contents_t contents_b;
	contents_b.edata = &edata_b;
	contents_b.metadata.szind = edata_szind_get_maybe_invalid(&edata_b);
	contents_b.metadata.slab = edata_slab_get(&edata_b);
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, ~((uintptr_t)0),
	    contents_b), "Unexpected rtree_write() failure");
	rtree_contents_t read_contents_b = rtree_read(tsdn, rtree, &rtree_ctx,
	    ~((uintptr_t)0));
	assert_true(contents_b.edata == read_contents_b.edata
	    && contents_b.metadata.szind == read_contents_b.metadata.szind
	    && contents_b.metadata.slab == read_contents_b.metadata.slab,
	    "rtree_read() should return previously set value");

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_rtree_bits) {
	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	uintptr_t keys[] = {PAGE, PAGE + 1,
	    PAGE + (((uintptr_t)1) << LG_PAGE) - 1};

	edata_t edata = {0};
	edata_init(&edata, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < sizeof(keys)/sizeof(uintptr_t); i++) {
		rtree_contents_t contents;
		contents.edata = &edata;
		contents.metadata.szind = SC_NSIZES;
		contents.metadata.slab = false;

		expect_false(rtree_write(tsdn, rtree, &rtree_ctx, keys[i],
		    contents), "Unexpected rtree_write() failure");
		for (unsigned j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
			    keys[j]).edata, &edata,
			    "rtree_edata_read() should return previously set "
			    "value and ignore insignificant key bits; i=%u, "
			    "j=%u, set key=%#"FMTxPTR", get key=%#"FMTxPTR, i,
			    j, keys[i], keys[j]);
		}
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    (((uintptr_t)2) << LG_PAGE)).edata,
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

	edata_t edata = {0};
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
		expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata, &edata,
		    "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata, &edata,
		    "rtree_edata_read() should return previously set value, "
		    "i=%u", i);
	}

	for (unsigned i = 0; i < NSET; i++) {
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata,
		   "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata,
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
