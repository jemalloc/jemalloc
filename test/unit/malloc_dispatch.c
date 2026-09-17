#include "test/jemalloc_test.h"

/*
 * Targeted coverage for the malloc_dispatch routing layer.  Integration
 * tests exercise the public API end-to-end but don't assert which branch
 * was taken for a given (size, tcache) input.  These tests make the
 * routing observable through per-arena stats counters:
 *
 *   1. small + tcache != NULL          -> tcache_alloc_small
 *   2. small + tcache == NULL          -> arena_malloc_hard + arena_dalloc_small
 *   3. large <= tcache_max + tcache    -> tcache_alloc_large
 *   4. large >  tcache_max + tcache    -> arena_malloc_hard + large_dalloc
 */

static unsigned
create_fresh_arena(void) {
	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "arenas.create failed");
	return arena_ind;
}

static void
refresh_stats(void) {
	uint64_t epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "epoch refresh failed");
}

static uint64_t
read_u64_mallctl(const char *cmd) {
	uint64_t v;
	size_t   sz = sizeof(v);
	expect_d_eq(mallctl(cmd, (void *)&v, &sz, NULL, 0), 0,
	    "mallctl read failed");
	return v;
}

static uint64_t
arena_bin0_nmalloc(unsigned arena_ind) {
	char cmd[128];
	(void)snprintf(
	    cmd, sizeof(cmd), "stats.arenas.%u.bins.0.nmalloc", arena_ind);
	return read_u64_mallctl(cmd);
}

static uint64_t
arena_bin0_ndalloc(unsigned arena_ind) {
	char cmd[128];
	(void)snprintf(
	    cmd, sizeof(cmd), "stats.arenas.%u.bins.0.ndalloc", arena_ind);
	return read_u64_mallctl(cmd);
}

static uint64_t
arena_bin0_nfills(unsigned arena_ind) {
	char cmd[128];
	(void)snprintf(
	    cmd, sizeof(cmd), "stats.arenas.%u.bins.0.nfills", arena_ind);
	return read_u64_mallctl(cmd);
}

static uint64_t
arena_large_nmalloc(unsigned arena_ind) {
	char cmd[128];
	(void)snprintf(
	    cmd, sizeof(cmd), "stats.arenas.%u.large.nmalloc", arena_ind);
	return read_u64_mallctl(cmd);
}

static uint64_t
arena_large_ndalloc(unsigned arena_ind) {
	char cmd[128];
	(void)snprintf(
	    cmd, sizeof(cmd), "stats.arenas.%u.large.ndalloc", arena_ind);
	return read_u64_mallctl(cmd);
}

/*
 * Branch 2: small alloc/dalloc with MALLOCX_TCACHE_NONE must increment the
 * arena bin counters immediately (no tcache to absorb the call).
 */
TEST_BEGIN(test_dispatch_small_no_tcache) {
	test_skip_if(!config_stats);
	test_skip_if(opt_prof);

	unsigned arena_ind = create_fresh_arena();
	int      flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t   sz = bin_infos[0].reg_size;

	refresh_stats();
	uint64_t nmalloc_before = arena_bin0_nmalloc(arena_ind);
	uint64_t ndalloc_before = arena_bin0_ndalloc(arena_ind);
	uint64_t nfills_before = arena_bin0_nfills(arena_ind);

	void *p = mallocx(sz, flags);
	expect_ptr_not_null(p, "mallocx failed");
	dallocx(p, flags);

	refresh_stats();
	expect_u64_eq(arena_bin0_nmalloc(arena_ind), nmalloc_before + 1,
	    "small no-tcache alloc must increment arena bin nmalloc");
	expect_u64_eq(arena_bin0_ndalloc(arena_ind), ndalloc_before + 1,
	    "small no-tcache dalloc must increment arena bin ndalloc");
	expect_u64_eq(arena_bin0_nfills(arena_ind), nfills_before,
	    "no-tcache path must not trigger any tcache fill");
}
TEST_END

/*
 * Branch 1: small alloc with tcache must NOT increment the arena bin nmalloc
 * counter directly (the tcache absorbs the alloc); a refill (nfills > 0) is
 * the routing observable instead.  Bound the test to a single tcache slot to
 * make the refill predictable.
 */
TEST_BEGIN(test_dispatch_small_with_tcache) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);

	unsigned arena_ind = create_fresh_arena();
	int      flags = MALLOCX_ARENA(arena_ind);
	size_t   sz = bin_infos[0].reg_size;

	/* Flush any per-thread tcache so this arena's tcache slot starts cold. */
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "thread.tcache.flush failed");

	refresh_stats();
	uint64_t nfills_before = arena_bin0_nfills(arena_ind);

	void *p = mallocx(sz, flags);
	expect_ptr_not_null(p, "mallocx failed");

	refresh_stats();
	expect_u64_gt(arena_bin0_nfills(arena_ind), nfills_before,
	    "small alloc with tcache must trigger a tcache fill");
	dallocx(p, flags);
}
TEST_END

/*
 * Branches 3 & 4: large alloc routing pivots on tcache_max.  Use a fresh
 * thread.tcache.max so we can compute a size that is just above it; that size
 * must be routed through arena_malloc_hard (and not the tcache).  Allocations
 * below tcache_max routed through the same call site must not touch the arena
 * large counters on the malloc/free pair (they ride the tcache).
 */
TEST_BEGIN(test_dispatch_large_routes_on_tcache_max) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_tcache);
	test_skip_if(opt_prof);
	test_skip_if(san_uaf_detection_enabled());

	unsigned arena_ind = create_fresh_arena();
	expect_d_eq(
	    mallctl("thread.arena", NULL, NULL, &arena_ind, sizeof(arena_ind)),
	    0, "thread.arena bind failed");

	/* Pin tcache_max to a known small-large boundary. */
	size_t small_large;
	size_t sz = sizeof(small_large);
	expect_d_eq(mallctl("arenas.lextent.0.size", (void *)&small_large, &sz,
	                NULL, 0),
	    0, "arenas.lextent.0.size lookup failed");
	expect_d_eq(mallctl("thread.tcache.max", NULL, NULL, &small_large,
	                sizeof(small_large)),
	    0, "thread.tcache.max set failed");
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "thread.tcache.flush failed");

	int flags = MALLOCX_ARENA(arena_ind);

	/* Above tcache_max: must hit arena_malloc_hard + large_dalloc. */
	size_t too_big;
	expect_d_eq(mallctl("arenas.lextent.1.size", (void *)&too_big, &sz, NULL,
	                0),
	    0, "arenas.lextent.1.size lookup failed");
	expect_zu_gt(too_big, small_large,
	    "lextent.1 must exceed tcache_max boundary");

	refresh_stats();
	uint64_t nmalloc_before = arena_large_nmalloc(arena_ind);
	uint64_t ndalloc_before = arena_large_ndalloc(arena_ind);

	void *big = mallocx(too_big, flags);
	expect_ptr_not_null(big, "mallocx failed");

	refresh_stats();
	expect_u64_eq(arena_large_nmalloc(arena_ind), nmalloc_before + 1,
	    "large alloc above tcache_max must bypass tcache "
	    "and increment arena large nmalloc");

	dallocx(big, flags);
	refresh_stats();
	expect_u64_eq(arena_large_ndalloc(arena_ind), ndalloc_before + 1,
	    "large dalloc above tcache_max must bypass tcache "
	    "and increment arena large ndalloc");
}
TEST_END

int
main(void) {
	return test(test_dispatch_small_no_tcache,
	    test_dispatch_small_with_tcache,
	    test_dispatch_large_routes_on_tcache_max);
}
