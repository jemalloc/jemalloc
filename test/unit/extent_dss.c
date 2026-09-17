#include "test/jemalloc_test.h"

#define SBRK_INVALID ((void *)-1)

static unsigned
create_arena(void) {
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);

	expect_d_eq(mallctl("arenas.create", &arena_ind, &sz, NULL, 0), 0,
	    "Unexpected arenas.create failure");
	return arena_ind;
}

static arena_t *
get_arena(unsigned arena_ind) {
	tsdn_t *tsdn = tsdn_fetch();
	arena_t *arena = arena_get(tsdn, arena_ind, false);
	expect_ptr_not_null(arena, "Unexpected arena_get failure");
	return arena;
}

static void
destroy_arena(unsigned arena_ind) {
	char arena_destroy[64];

	malloc_snprintf(arena_destroy, sizeof(arena_destroy), "arena.%u.destroy",
	    arena_ind);
	expect_d_eq(mallctl(arena_destroy, NULL, NULL, NULL, 0), 0,
	    "Unexpected arena destroy failure");
}

TEST_BEGIN(test_dss_primary_alloc_real_sbrk) {
	test_skip_if(!have_dss);
	test_skip_if(opt_hpa);

	unsigned arena_ind = create_arena();
	arena_t *arena = get_arena(arena_ind);
	expect_false(arena_dss_prec_set(arena, dss_prec_primary),
	    "Unexpected arena_dss_prec_set failure");

	size_t size = SC_LARGE_MINCLASS;
	size_t alignment = PAGE << 4;
	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE |
	    MALLOCX_ALIGN(alignment);
	void *ptr = mallocx(size, flags);
	expect_ptr_not_null(ptr, "Unexpected mallocx failure");
	expect_zu_eq((uintptr_t)ptr & (alignment - 1), 0,
	    "Unexpected DSS allocation alignment");
	expect_true(extent_in_dss(ptr), "Expected primary DSS allocation");

	dallocx(ptr, flags);
	destroy_arena(arena_ind);
}
TEST_END

static void *mock_dss_cur;
static bool mock_dss_race_once;
static bool mock_dss_oom;
static unsigned mock_dss_calls;
static unsigned mock_dss_nonzero_calls;

static void *
mock_dss_base(void) {
	return (void *)(uintptr_t)(PAGE * 1024);
}

static void
mock_dss_reset(bool race_once, bool oom) {
	mock_dss_cur = mock_dss_base();
	mock_dss_race_once = race_once;
	mock_dss_oom = oom;
	mock_dss_calls = 0;
	mock_dss_nonzero_calls = 0;
	extent_dss_sbrk_hook = NULL;
}

static void *
mock_dss_sbrk(intptr_t increment) {
	mock_dss_calls++;
	if (increment == 0) {
		return mock_dss_cur;
	}

	mock_dss_nonzero_calls++;
	if (mock_dss_oom) {
		return SBRK_INVALID;
	}

	void *ret = mock_dss_cur;
	if (mock_dss_race_once) {
		mock_dss_race_once = false;
		void *raced_cur = (void *)((byte_t *)mock_dss_cur + PAGE);
		assert_true(raced_cur > mock_dss_cur,
		    "Unexpected mock DSS race address overflow");
		mock_dss_cur = raced_cur;
		ret = mock_dss_cur;
	}
	mock_dss_cur = (void *)((byte_t *)mock_dss_cur + increment);
	return ret;
}

static void
mock_dss_boot(bool race_once, bool oom) {
	mock_dss_reset(race_once, oom);
	extent_dss_sbrk_hook = mock_dss_sbrk;
	extent_dss_boot();
	expect_u_eq(mock_dss_calls, 1, "Expected DSS boot sbrk(0)");
}

static void
mock_dss_restore_real(void) {
	extent_dss_sbrk_hook = NULL;
	extent_dss_boot();
}

static void *
alloc_dss(unsigned arena_ind, void *new_addr, size_t size, size_t alignment) {
	bool zero = false;
	bool commit = true;
	return extent_alloc_dss(tsdn_fetch(), get_arena(arena_ind), new_addr,
	    size, alignment, &zero, &commit);
}

TEST_BEGIN(test_dss_rejects_negative_sbrk_size) {
	test_skip_if(!have_dss);

	unsigned arena_ind = create_arena();
	void *ret = alloc_dss(arena_ind, NULL, ((size_t)1 << (sizeof(size_t) *
	    8 - 1)), PAGE);
	expect_ptr_null(ret, "Expected too-large DSS allocation to fail");
	destroy_arena(arena_ind);
}
TEST_END

TEST_BEGIN(test_dss_rejects_non_edge_fixed_addr) {
	test_skip_if(!have_dss);

	unsigned arena_ind = create_arena();
	mock_dss_boot(/* race_once */ false, /* oom */ false);

	void *bad_addr = (void *)((byte_t *)mock_dss_base() + PAGE);
	void *ret = alloc_dss(arena_ind, bad_addr, PAGE, PAGE);
	expect_ptr_null(ret, "Expected non-edge fixed-address DSS alloc failure");
	expect_u_eq(mock_dss_nonzero_calls, 0,
	    "Non-edge fixed-address failure should not extend DSS");

	mock_dss_restore_real();
	destroy_arena(arena_ind);
}
TEST_END

TEST_BEGIN(test_dss_retries_after_sbrk_race) {
	test_skip_if(!have_dss);

	unsigned arena_ind = create_arena();
	mock_dss_boot(/* race_once */ true, /* oom */ false);

	void *ret = alloc_dss(arena_ind, NULL, PAGE, PAGE);
	expect_ptr_not_null(ret, "Expected DSS allocation after sbrk race");
	expect_u_eq(mock_dss_nonzero_calls, 2,
	    "Expected one raced sbrk extension and one retry");

	mock_dss_restore_real();
	destroy_arena(arena_ind);
}
TEST_END

TEST_BEGIN(test_dss_exhausted_is_sticky) {
	test_skip_if(!have_dss);

	unsigned arena_ind = create_arena();
	mock_dss_boot(/* race_once */ false, /* oom */ true);

	void *ret = alloc_dss(arena_ind, NULL, PAGE, PAGE);
	expect_ptr_null(ret, "Expected DSS allocation failure");
	expect_u_eq(mock_dss_nonzero_calls, 1, "Expected one failed extension");

	unsigned calls_before = mock_dss_calls;
	ret = alloc_dss(arena_ind, NULL, PAGE, PAGE);
	expect_ptr_null(ret, "Expected exhausted DSS allocation failure");
	expect_u_eq(mock_dss_calls, calls_before,
	    "Exhausted DSS should fail without calling sbrk again");

	mock_dss_restore_real();
	destroy_arena(arena_ind);
}
TEST_END

int
main(void) {
	return test(test_dss_primary_alloc_real_sbrk,
	    test_dss_rejects_negative_sbrk_size,
	    test_dss_rejects_non_edge_fixed_addr,
	    test_dss_retries_after_sbrk_race, test_dss_exhausted_is_sticky);
}
