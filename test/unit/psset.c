#include "test/jemalloc_test.h"

#include "jemalloc/internal/psset.h"

#define PAGESLAB_PAGES 64
#define PAGESLAB_SIZE (PAGESLAB_PAGES << LG_PAGE)
#define PAGESLAB_SN 123
#define PAGESLAB_ADDR ((void *)(1234 << LG_PAGE))

#define ALLOC_ARENA_IND 111
#define ALLOC_ESN 222

static void
edata_init_test(edata_t *edata) {
	memset(edata, 0, sizeof(*edata));
	edata_arena_ind_set(edata, ALLOC_ARENA_IND);
	edata_esn_set(edata, ALLOC_ESN);
}

static void
edata_expect(edata_t *edata, size_t page_offset, size_t page_cnt) {
	/*
	 * Note that allocations should get the arena ind of their home
	 * arena, *not* the arena ind of the pageslab allocator.
	 */
	expect_u_eq(ALLOC_ARENA_IND, edata_arena_ind_get(edata),
	    "Arena ind changed");
	expect_ptr_eq(
	    (void *)((uintptr_t)PAGESLAB_ADDR + (page_offset << LG_PAGE)),
	    edata_addr_get(edata), "Didn't allocate in order");
	expect_zu_eq(page_cnt << LG_PAGE, edata_size_get(edata), "");
	expect_false(edata_slab_get(edata), "");
	expect_u_eq(SC_NSIZES, edata_szind_get_maybe_invalid(edata),
	    "");
	expect_zu_eq(0, edata_sn_get(edata), "");
	expect_d_eq(edata_state_get(edata), extent_state_active, "");
	expect_false(edata_zeroed_get(edata), "");
	expect_true(edata_committed_get(edata), "");
	expect_d_eq(EXTENT_PAI_HPA, edata_pai_get(edata), "");
	expect_false(edata_is_head_get(edata), "");
}

TEST_BEGIN(test_empty) {
	bool err;
	edata_t pageslab;
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc;

	edata_init(&pageslab, /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);
	edata_init_test(&alloc);

	psset_t psset;
	psset_init(&psset);

	/* Empty psset should return fail allocations. */
	err = psset_alloc_reuse(&psset, &alloc, PAGE);
	expect_true(err, "Empty psset succeeded in an allocation.");
}
TEST_END

TEST_BEGIN(test_fill) {
	bool err;
	edata_t pageslab;
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc[PAGESLAB_PAGES];

	edata_init(&pageslab, /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);

	psset_t psset;
	psset_init(&psset);

	edata_init_test(&alloc[0]);
	psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < PAGESLAB_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}

	for (size_t i = 0; i < PAGESLAB_PAGES; i++) {
		edata_t *edata = &alloc[i];
		edata_expect(edata, i, 1);
	}

	/* The pageslab, and thus psset, should now have no allocations. */
	edata_t extra_alloc;
	edata_init_test(&extra_alloc);
	err = psset_alloc_reuse(&psset, &extra_alloc, PAGE);
	expect_true(err, "Alloc succeeded even though psset should be empty");
}
TEST_END

TEST_BEGIN(test_reuse) {
	bool err;
	edata_t *ps;

	edata_t pageslab;
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc[PAGESLAB_PAGES];

	edata_init(&pageslab, /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);

	psset_t psset;
	psset_init(&psset);

	edata_init_test(&alloc[0]);
	psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < PAGESLAB_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}

	/* Free odd indices. */
	for (size_t i = 0; i < PAGESLAB_PAGES; i ++) {
		if (i % 2 == 0) {
			continue;
		}
		ps = psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/* Realloc into them. */
	for (size_t i = 0; i < PAGESLAB_PAGES; i++) {
		if (i % 2 == 0) {
			continue;
		}
		err = psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		edata_expect(&alloc[i], i, 1);
	}
	/* Now, free the pages at indices 0 or 1 mod 2. */
	for (size_t i = 0; i < PAGESLAB_PAGES; i++) {
		if (i % 4 > 1) {
			continue;
		}
		ps = psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/* And realloc 2-page allocations into them. */
	for (size_t i = 0; i < PAGESLAB_PAGES; i++) {
		if (i % 4 != 0) {
			continue;
		}
		err = psset_alloc_reuse(&psset, &alloc[i], 2 * PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		edata_expect(&alloc[i], i, 2);
	}
	/* Free all the 2-page allocations. */
	for (size_t i = 0; i < PAGESLAB_PAGES; i++) {
		if (i % 4 != 0) {
			continue;
		}
		ps = psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	/*
	 * Free up a 1-page hole next to a 2-page hole, but somewhere in the
	 * middle of the pageslab.  Index 11 should be right before such a hole
	 * (since 12 % 4 == 0).
	 */
	size_t index_of_3 = 11;
	ps = psset_dalloc(&psset, &alloc[index_of_3]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");
	err = psset_alloc_reuse(&psset, &alloc[index_of_3], 3 * PAGE);
	expect_false(err, "Should have been able to find alloc.");
	edata_expect(&alloc[index_of_3], index_of_3, 3);

	/* Free up a 4-page hole at the end. */
	ps = psset_dalloc(&psset, &alloc[PAGESLAB_PAGES - 1]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");
	ps = psset_dalloc(&psset, &alloc[PAGESLAB_PAGES - 2]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");

	/* Make sure we can satisfy an allocation at the very end of a slab. */
	size_t index_of_4 = PAGESLAB_PAGES - 4;
	ps = psset_dalloc(&psset, &alloc[index_of_4]);
	expect_ptr_null(ps, "Nonempty pageslab evicted");
	err = psset_alloc_reuse(&psset, &alloc[index_of_4], 4 * PAGE);
	expect_false(err, "Should have been able to find alloc.");
	edata_expect(&alloc[index_of_4], index_of_4, 4);
}
TEST_END

TEST_BEGIN(test_evict) {
	bool err;
	edata_t *ps;
	edata_t pageslab;
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc[PAGESLAB_PAGES];

	edata_init(&pageslab, /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);
	psset_t psset;
	psset_init(&psset);

	/* Alloc the whole slab. */
	edata_init_test(&alloc[0]);
	psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < PAGESLAB_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Unxpected allocation failure");
	}

	/* Dealloc the whole slab, going forwards. */
	for (size_t i = 0; i < PAGESLAB_PAGES - 1; i++) {
		ps = psset_dalloc(&psset, &alloc[i]);
		expect_ptr_null(ps, "Nonempty pageslab evicted");
	}
	ps = psset_dalloc(&psset, &alloc[PAGESLAB_PAGES - 1]);
	expect_ptr_eq(&pageslab, ps, "Empty pageslab not evicted.");

	err = psset_alloc_reuse(&psset, &alloc[0], PAGE);
	expect_true(err, "psset should be empty.");
}
TEST_END

TEST_BEGIN(test_multi_pageslab) {
	bool err;
	edata_t *ps;
	edata_t pageslab[2];
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc[2][PAGESLAB_PAGES];

	edata_init(&pageslab[0], /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);
	edata_init(&pageslab[1], /* arena_ind */ 0,
	    (void *)((uintptr_t)PAGESLAB_ADDR + PAGESLAB_SIZE), PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);

	psset_t psset;
	psset_init(&psset);

	/* Insert both slabs. */
	edata_init_test(&alloc[0][0]);
	psset_alloc_new(&psset, &pageslab[0], &alloc[0][0], PAGE);
	edata_init_test(&alloc[1][0]);
	psset_alloc_new(&psset, &pageslab[1], &alloc[1][0], PAGE);

	/* Fill them both up; make sure we do so in first-fit order. */
	for (size_t i = 0; i < 2; i++) {
		for (size_t j = 1; j < PAGESLAB_PAGES; j++) {
			edata_init_test(&alloc[i][j]);
			err = psset_alloc_reuse(&psset, &alloc[i][j], PAGE);
			expect_false(err,
			    "Nonempty psset failed page allocation.");
			assert_ptr_eq(&pageslab[i], edata_ps_get(&alloc[i][j]),
			    "Didn't pick pageslabs in first-fit");
		}
	}

	/*
	 * Free up a 2-page hole in the earlier slab, and a 1-page one in the
	 * later one.  We should still pick the later one.
	 */
	ps = psset_dalloc(&psset, &alloc[0][0]);
	expect_ptr_null(ps, "Unexpected eviction");
	ps = psset_dalloc(&psset, &alloc[0][1]);
	expect_ptr_null(ps, "Unexpected eviction");
	ps = psset_dalloc(&psset, &alloc[1][0]);
	expect_ptr_null(ps, "Unexpected eviction");
	err = psset_alloc_reuse(&psset, &alloc[0][0], PAGE);
	expect_ptr_eq(&pageslab[1], edata_ps_get(&alloc[0][0]),
	    "Should have picked the fuller pageslab");

	/*
	 * Now both slabs have 1-page holes. Free up a second one in the later
	 * slab.
	 */
	ps = psset_dalloc(&psset, &alloc[1][1]);
	expect_ptr_null(ps, "Unexpected eviction");

	/*
	 * We should be able to allocate a 2-page object, even though an earlier
	 * size class is nonempty.
	 */
	err = psset_alloc_reuse(&psset, &alloc[1][0], 2 * PAGE);
	expect_false(err, "Allocation should have succeeded");
}
TEST_END

static void
stats_expect_empty(psset_bin_stats_t *stats) {
	assert_zu_eq(0, stats->npageslabs,
	    "Supposedly empty bin had positive npageslabs");
	expect_zu_eq(0, stats->nactive, "Unexpected nonempty bin"
	    "Supposedly empty bin had positive nactive");
	expect_zu_eq(0, stats->ninactive, "Unexpected nonempty bin"
	    "Supposedly empty bin had positive ninactive");
}

static void
stats_expect(psset_t *psset, size_t nactive) {
	if (nactive == PAGESLAB_PAGES) {
		expect_zu_eq(1, psset->full_slab_stats.npageslabs,
		    "Expected a full slab");
		expect_zu_eq(PAGESLAB_PAGES, psset->full_slab_stats.nactive,
		    "Should have exactly filled the bin");
		expect_zu_eq(0, psset->full_slab_stats.ninactive,
		    "Should never have inactive pages in a full slab");
	} else {
		stats_expect_empty(&psset->full_slab_stats);
	}
	size_t ninactive = PAGESLAB_PAGES - nactive;
	pszind_t nonempty_pind = PSSET_NPSIZES;
	if (ninactive != 0 && ninactive < PAGESLAB_PAGES) {
		nonempty_pind = sz_psz2ind(sz_psz_quantize_floor(
		    ninactive << LG_PAGE));
	}
	for (pszind_t i = 0; i < PSSET_NPSIZES; i++) {
		if (i == nonempty_pind) {
			assert_zu_eq(1, psset->slab_stats[i].npageslabs,
			    "Should have found a slab");
			expect_zu_eq(nactive, psset->slab_stats[i].nactive,
			    "Mismatch in active pages");
			expect_zu_eq(ninactive, psset->slab_stats[i].ninactive,
			    "Mismatch in inactive pages");
		} else {
			stats_expect_empty(&psset->slab_stats[i]);
		}
	}
}

TEST_BEGIN(test_stats) {
	bool err;
	edata_t pageslab;
	memset(&pageslab, 0, sizeof(pageslab));
	edata_t alloc[PAGESLAB_PAGES];

	edata_init(&pageslab, /* arena_ind */ 0, PAGESLAB_ADDR, PAGESLAB_SIZE,
	    /* slab */ true, SC_NSIZES, PAGESLAB_SN, extent_state_active,
	    /* zeroed */ false, /* comitted */ true, EXTENT_PAI_HPA,
	    EXTENT_IS_HEAD);

	psset_t psset;
	psset_init(&psset);
	stats_expect(&psset, 0);

	edata_init_test(&alloc[0]);
	psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < PAGESLAB_PAGES; i++) {
		stats_expect(&psset, i);
		edata_init_test(&alloc[i]);
		err = psset_alloc_reuse(&psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
	}
	stats_expect(&psset, PAGESLAB_PAGES);
	edata_t *ps;
	for (ssize_t i = PAGESLAB_PAGES - 1; i >= 0; i--) {
		ps = psset_dalloc(&psset, &alloc[i]);
		expect_true((ps == NULL) == (i != 0),
		    "psset_dalloc should only evict a slab on the last free");
		stats_expect(&psset, i);
	}

	psset_alloc_new(&psset, &pageslab, &alloc[0], PAGE);
	stats_expect(&psset, 1);
	psset_remove(&psset, &pageslab);
	stats_expect(&psset, 0);
	psset_insert(&psset, &pageslab);
	stats_expect(&psset, 1);
}
TEST_END

/*
 * Fills in and inserts two pageslabs, with the first better than the second,
 * and each fully allocated (into the allocations in allocs and worse_allocs,
 * each of which should be PAGESLAB_PAGES long).
 *
 * (There's nothing magic about these numbers; it's just useful to share the
 * setup between the oldest fit and the insert/remove test).
 */
static void
init_test_pageslabs(psset_t *psset, edata_t *pageslab, edata_t *worse_pageslab,
    edata_t *alloc, edata_t *worse_alloc) {
	bool err;
	memset(pageslab, 0, sizeof(*pageslab));
	edata_init(pageslab, /* arena_ind */ 0, (void *)(10 * PAGESLAB_SIZE),
	    PAGESLAB_SIZE, /* slab */ true, SC_NSIZES, PAGESLAB_SN + 1,
	    extent_state_active, /* zeroed */ false, /* comitted */ true,
	    EXTENT_PAI_HPA, EXTENT_IS_HEAD);

	/*
	 * This pageslab is better from an edata_comp_snad POV, but will be
	 * added to the set after the previous one, and so should be less
	 * preferred for allocations.
	 */
	memset(worse_pageslab, 0, sizeof(*worse_pageslab));
	edata_init(worse_pageslab, /* arena_ind */ 0,
	    (void *)(9 * PAGESLAB_SIZE), PAGESLAB_SIZE, /* slab */ true,
	    SC_NSIZES, PAGESLAB_SN - 1, extent_state_active, /* zeroed */ false,
	    /* comitted */ true, EXTENT_PAI_HPA, EXTENT_IS_HEAD);

	psset_init(psset);

	edata_init_test(&alloc[0]);
	psset_alloc_new(psset, pageslab, &alloc[0], PAGE);
	for (size_t i = 1; i < PAGESLAB_PAGES; i++) {
		edata_init_test(&alloc[i]);
		err = psset_alloc_reuse(psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		expect_ptr_eq(pageslab, edata_ps_get(&alloc[i]),
		    "Allocated from the wrong pageslab");
	}

	edata_init_test(&worse_alloc[0]);
	psset_alloc_new(psset, worse_pageslab, &worse_alloc[0], PAGE);
	expect_ptr_eq(worse_pageslab, edata_ps_get(&worse_alloc[0]),
	    "Allocated from the wrong pageslab");
	/*
	 * Make the two pssets otherwise indistinguishable; all full except for
	 * a single page.
	 */
	for (size_t i = 1; i < PAGESLAB_PAGES - 1; i++) {
		edata_init_test(&worse_alloc[i]);
		err = psset_alloc_reuse(psset, &alloc[i], PAGE);
		expect_false(err, "Nonempty psset failed page allocation.");
		expect_ptr_eq(worse_pageslab, edata_ps_get(&alloc[i]),
		    "Allocated from the wrong pageslab");
	}

	/* Deallocate the last page from the older pageslab. */
	edata_t *evicted = psset_dalloc(psset, &alloc[PAGESLAB_PAGES - 1]);
	expect_ptr_null(evicted, "Unexpected eviction");
}

TEST_BEGIN(test_oldest_fit) {
	bool err;
	edata_t alloc[PAGESLAB_PAGES];
	edata_t worse_alloc[PAGESLAB_PAGES];

	edata_t pageslab;
	edata_t worse_pageslab;

	psset_t psset;

	init_test_pageslabs(&psset, &pageslab, &worse_pageslab, alloc,
	    worse_alloc);

	/* The edata should come from the better pageslab. */
	edata_t test_edata;
	edata_init_test(&test_edata);
	err = psset_alloc_reuse(&psset, &test_edata, PAGE);
	expect_false(err, "Nonempty psset failed page allocation");
	expect_ptr_eq(&pageslab, edata_ps_get(&test_edata),
	    "Allocated from the wrong pageslab");
}
TEST_END

TEST_BEGIN(test_insert_remove) {
	bool err;
	edata_t *ps;
	edata_t alloc[PAGESLAB_PAGES];
	edata_t worse_alloc[PAGESLAB_PAGES];

	edata_t pageslab;
	edata_t worse_pageslab;

	psset_t psset;

	init_test_pageslabs(&psset, &pageslab, &worse_pageslab, alloc,
	    worse_alloc);

	/* Remove better; should still be able to alloc from worse. */
	psset_remove(&psset, &pageslab);
	err = psset_alloc_reuse(&psset, &worse_alloc[PAGESLAB_PAGES - 1], PAGE);
	expect_false(err, "Removal should still leave an empty page");
	expect_ptr_eq(&worse_pageslab,
	    edata_ps_get(&worse_alloc[PAGESLAB_PAGES - 1]),
	    "Allocated out of wrong ps");

	/*
	 * After deallocating the previous alloc and reinserting better, it
	 * should be preferred for future allocations.
	 */
	ps = psset_dalloc(&psset, &worse_alloc[PAGESLAB_PAGES - 1]);
	expect_ptr_null(ps, "Incorrect eviction of nonempty pageslab");
	psset_insert(&psset, &pageslab);
	err = psset_alloc_reuse(&psset, &alloc[PAGESLAB_PAGES - 1], PAGE);
	expect_false(err, "psset should be nonempty");
	expect_ptr_eq(&pageslab, edata_ps_get(&alloc[PAGESLAB_PAGES - 1]),
	    "Removal/reinsertion shouldn't change ordering");
	/*
	 * After deallocating and removing both, allocations should fail.
	 */
	ps = psset_dalloc(&psset, &alloc[PAGESLAB_PAGES - 1]);
	expect_ptr_null(ps, "Incorrect eviction");
	psset_remove(&psset, &pageslab);
	psset_remove(&psset, &worse_pageslab);
	err = psset_alloc_reuse(&psset, &alloc[PAGESLAB_PAGES - 1], PAGE);
	expect_true(err, "psset should be empty, but an alloc succeeded");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_empty,
	    test_fill,
	    test_reuse,
	    test_evict,
	    test_multi_pageslab,
	    test_stats,
	    test_oldest_fit,
	    test_insert_remove);
}
