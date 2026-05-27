#include "test/jemalloc_test.h"

#include "jemalloc/internal/eset.h"

#define ESET_TEST_ARENA_IND 111
#define ESET_TEST_ADDR_BASE ((uintptr_t)0x30000000U)

static void
test_edata_init(edata_t *edata, uintptr_t addr, size_t size, uint64_t sn,
    extent_state_t state, bool pinned) {
	memset(edata, 0, sizeof(*edata));
	edata_init(edata, ESET_TEST_ARENA_IND, (void *)addr, size,
	    /* slab */ false, SC_NSIZES, sn, state, /* zeroed */ false,
	    /* committed */ true, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	edata_pinned_set(edata, pinned);
}

static void
test_eset_init(eset_t *eset, extent_state_t state) {
	memset(eset, 0, sizeof(*eset));
	eset_init(eset, state);
}

TEST_BEGIN(test_eset_insert_remove_fit) {
	eset_t eset;
	test_eset_init(&eset, extent_state_dirty);

	edata_t a;
	edata_t b;
	edata_t c;
	edata_t pinned;
	test_edata_init(&a, ESET_TEST_ADDR_BASE, 2 * PAGE, 20,
	    extent_state_dirty, false);
	test_edata_init(&b, ESET_TEST_ADDR_BASE + HUGEPAGE, 2 * PAGE, 10,
	    extent_state_dirty, false);
	test_edata_init(&c, ESET_TEST_ADDR_BASE + 2 * HUGEPAGE, 4 * PAGE, 5,
	    extent_state_dirty, false);
	test_edata_init(&pinned, ESET_TEST_ADDR_BASE + 3 * HUGEPAGE, PAGE, 1,
	    extent_state_dirty, true);

	eset_insert(&eset, &a);
	eset_insert(&eset, &b);
	eset_insert(&eset, &c);
	eset_insert(&eset, &pinned);

	expect_zu_eq(9, eset_npages_get(&eset),
	    "Unexpected page count after inserts");
	if (config_stats) {
		pszind_t pind_2p = sz_psz2ind(
		    sz_psz_quantize_floor(2 * PAGE));
		expect_zu_eq(2, eset_nextents_get(&eset, pind_2p),
		    "Unexpected extent count in 2-page bin");
		expect_zu_eq(4 * PAGE, eset_nbytes_get(&eset, pind_2p),
		    "Unexpected byte count in 2-page bin");
	}

	expect_ptr_eq(&a, edata_list_inactive_first(&eset.lru),
	    "Non-pinned extents should keep insertion LRU order");
	expect_ptr_eq(&b, edata_list_inactive_next(&eset.lru, &a),
	    "Non-pinned extents should keep insertion LRU order");
	expect_ptr_eq(&c, edata_list_inactive_next(&eset.lru, &b),
	    "Pinned extents should be excluded from the LRU");

	edata_t *fit = eset_fit(&eset, 2 * PAGE, PAGE,
	    /* exact_only */ false, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&c, fit,
	    "Default first-fit should choose the oldest fitting extent across "
	    "larger bins");
	fit = eset_fit(&eset, 2 * PAGE, PAGE,
	    /* exact_only */ true, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&b, fit,
	    "Exact fit should choose the lowest serial number in the size bin");

	eset_remove(&eset, &b);
	expect_zu_eq(7, eset_npages_get(&eset),
	    "Unexpected page count after remove");
	fit = eset_fit(&eset, 2 * PAGE, PAGE, /* exact_only */ true,
	    SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&a, fit,
	    "Removing the heap min should refresh the bin summary");

	eset_remove(&eset, &pinned);
	expect_zu_eq(6, eset_npages_get(&eset),
	    "Pinned removal should still update page counts");
	expect_ptr_eq(&a, edata_list_inactive_first(&eset.lru),
	    "Pinned removal should not disturb LRU contents");

	eset_t prefer_eset;
	test_eset_init(&prefer_eset, extent_state_dirty);
	edata_t small_new;
	edata_t large_old;
	test_edata_init(&small_new, ESET_TEST_ADDR_BASE + 4 * HUGEPAGE,
	    4 * PAGE, 100, extent_state_dirty, false);
	test_edata_init(&large_old, ESET_TEST_ADDR_BASE + 5 * HUGEPAGE,
	    8 * PAGE, 1, extent_state_dirty, false);
	eset_insert(&prefer_eset, &small_new);
	eset_insert(&prefer_eset, &large_old);

	fit = eset_fit(&prefer_eset, 3 * PAGE, PAGE,
	    /* exact_only */ false, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&large_old, fit,
	    "Default first-fit should prefer the oldest suitable extent");
	fit = eset_fit(&prefer_eset, 3 * PAGE, PAGE,
	    /* exact_only */ false, SC_PTR_BITS, /* prefer_small */ true);
	expect_ptr_eq(&small_new, fit,
	    "prefer_small should stop at the smallest fitting bin");
}
TEST_END

TEST_BEGIN(test_eset_alignment_and_large_class_fallback) {
	eset_t eset;
	test_eset_init(&eset, extent_state_dirty);

	edata_t aligned_candidate;
	test_edata_init(&aligned_candidate,
	    ESET_TEST_ADDR_BASE + 2 * PAGE, 4 * PAGE, 1, extent_state_dirty,
	    false);
	eset_insert(&eset, &aligned_candidate);

	edata_t *fit = eset_fit(&eset, 2 * PAGE, 4 * PAGE,
	    /* exact_only */ false, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&aligned_candidate, fit,
	    "Alignment fallback should find a smaller extent that crosses the "
	    "requested alignment");

	eset_t max_fit_eset;
	test_eset_init(&max_fit_eset, extent_state_dirty);
	edata_t too_large_old;
	edata_t bounded_new;
	test_edata_init(&too_large_old, ESET_TEST_ADDR_BASE + 6 * HUGEPAGE,
	    64 * PAGE, 1, extent_state_dirty, false);
	test_edata_init(&bounded_new, ESET_TEST_ADDR_BASE + 7 * HUGEPAGE,
	    4 * PAGE, 100, extent_state_dirty, false);
	eset_insert(&max_fit_eset, &too_large_old);
	eset_insert(&max_fit_eset, &bounded_new);
	fit = eset_fit(&max_fit_eset, 2 * PAGE, PAGE,
	    /* exact_only */ false, /* lg_max_fit */ 1,
	    /* prefer_small */ false);
	expect_ptr_eq(&bounded_new, fit,
	    "lg_max_fit should reject excessively large older extents");
}
TEST_END

TEST_BEGIN(test_eset_exact_fit_large_class_disabled) {
	test_skip_if(!sz_large_size_classes_disabled());

	eset_t exact_eset;
	test_eset_init(&exact_eset, extent_state_dirty);
	size_t request = SC_LARGE_MINCLASS + PAGE;
	edata_t exact;
	edata_t larger;
	test_edata_init(&exact, ESET_TEST_ADDR_BASE + 8 * HUGEPAGE,
	    request, 2, extent_state_dirty, false);
	test_edata_init(&larger, ESET_TEST_ADDR_BASE + 9 * HUGEPAGE,
	    request + PAGE, 1, extent_state_dirty, false);
	eset_insert(&exact_eset, &larger);
	eset_insert(&exact_eset, &exact);

	edata_t *fit = eset_fit(&exact_eset, request, PAGE,
	    /* exact_only */ true, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_eq(&exact, fit,
	    "Exact search should enumerate the floor bin when large size "
	    "classes are disabled");
	fit = eset_fit(&exact_eset, request - PAGE, PAGE,
	    /* exact_only */ true, SC_PTR_BITS, /* prefer_small */ false);
	expect_ptr_null(fit,
	    "Exact search should not return merely larger extents");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_eset_insert_remove_fit,
	    test_eset_alignment_and_large_class_fallback,
	    test_eset_exact_fit_large_class_disabled);
}
