#include "test/jemalloc_test.h"

/*
 * Use 1 shard for deterministic stat assertions.  max_alloc is set to
 * USIZE_GROW_SLOW_THRESHOLD on every supported page size, which is the
 * largest value the conf parser will accept (it is also clipped down to
 * the per-platform maximum, so a single literal works across platforms).
 * SC_LARGE_MINCLASS == 1 << (LG_PAGE + SC_LG_NGROUP) is always strictly
 * less than USIZE_GROW_SLOW_THRESHOLD == 1 << (LG_PAGE + SC_LG_NGROUP +
 * 1), so SEC will always cover the alloc size used below.  max_bytes is
 * then sized to hold a small number of those extents so overflow
 * triggers quickly.  Background threads disabled to prevent asynchronous
 * decay from interfering with precise stat checks.
 */
const char *malloc_conf =
    "pac_sec_nshards:1,pac_sec_max_alloc:524288,"
    "pac_sec_max_bytes:2097152,background_thread:false";

static size_t
read_stat(unsigned arena_ind, const char *field) {
	char   cmd[128];
	size_t val;
	uint64_t epoch = 1;
	size_t sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sz), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(val);
	snprintf(cmd, sizeof(cmd), "stats.arenas.%u.pac_sec_%s",
	    arena_ind, field);
	expect_d_eq(mallctl(cmd, (void *)&val, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure reading pac_sec stat");
	return val;
}

/*
 * Pick a SEC-eligible alloc size and verify the runtime SEC config will
 * actually absorb it.  Fails loudly if SEC is unexpectedly bypassed
 * (e.g. nshards=0 or the conf parser clipped max_alloc below
 * SC_LARGE_MINCLASS), so test failures point at the precondition rather
 * than a downstream stat assertion.
 */
static size_t
sec_eligible_alloc_size(void) {
	size_t alloc_size = SC_LARGE_MINCLASS;

	size_t nshards;
	size_t sz = sizeof(nshards);
	expect_d_eq(mallctl("opt.pac_sec_nshards", (void *)&nshards,
	    &sz, NULL, 0), 0, "opt.pac_sec_nshards read failed");
	expect_zu_gt(nshards, 0,
	    "test precondition: pac_sec_nshards must be > 0");

	size_t max_alloc;
	sz = sizeof(max_alloc);
	expect_d_eq(mallctl("opt.pac_sec_max_alloc", (void *)&max_alloc,
	    &sz, NULL, 0), 0, "opt.pac_sec_max_alloc read failed");
	expect_zu_ge(max_alloc, alloc_size,
	    "precondition: pac_sec_max_alloc must cover SC_LARGE_MINCLASS");

	return alloc_size;
}

TEST_BEGIN(test_pac_sec_alloc_dalloc_cycle) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");

	int    flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = sec_eligible_alloc_size();

	size_t max_bytes;
	sz = sizeof(max_bytes);
	expect_d_eq(mallctl("opt.pac_sec_max_bytes",
	    (void *)&max_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	size_t capacity = max_bytes / alloc_size;
	expect_zu_gt(capacity, 0, "SEC capacity must be > 0 for this test");

	/* Step 1: First alloc - SEC miss, served from ecache or new mapping. */
	void *p1 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p1, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "misses"), 1,
	    "first alloc should miss SEC");
	expect_zu_eq(read_stat(arena_ind, "hits"), 0,
	    "no hits yet");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty (extent is active)");

	/* Step 2: Free p1 - SEC absorbs without flush. */
	dallocx(p1, flags);
	size_t cached_after_one = read_stat(arena_ind, "bytes");
	expect_zu_gt(cached_after_one, 0,
	    "SEC should cache the freed extent");
	/* Actual extent may exceed alloc_size due to size class rounding. */
	size_t extent_size = cached_after_one;
	expect_zu_eq(read_stat(arena_ind, "dalloc_noflush"), 1,
	    "one dalloc absorbed without flush");
	expect_zu_eq(read_stat(arena_ind, "dalloc_flush"), 0,
	    "no flush yet");

	/* Recompute capacity based on actual extent size. */
	capacity = max_bytes / extent_size;
	expect_zu_gt(capacity, 0, "SEC capacity should be positive");

	/* Step 3: Re-alloc same size - SEC hit, reuses cached extent. */
	void *p2 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p2, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "hits"), 1,
	    "second alloc should hit SEC");
	expect_zu_eq(read_stat(arena_ind, "misses"), 1,
	    "misses should not increase");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after hit");

	dallocx(p2, flags);

	/*
	 * Step 4: Allocate (capacity + 2) extents, then free them all.
	 * The first `capacity` frees fill SEC; remaining frees overflow
	 * and flush cold extents back to the ecaches.
	 */
	size_t nallocs = capacity + 2;
	void **ptrs = mallocx(nallocs * sizeof(void *),
	    MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(ptrs, "metadata alloc failed");

	for (size_t i = 0; i < nallocs; i++) {
		ptrs[i] = mallocx(alloc_size, flags);
		expect_ptr_not_null(ptrs[i], "mallocx %zu failed", i);
	}
	for (size_t i = 0; i < nallocs; i++) {
		dallocx(ptrs[i], flags);
	}

	size_t noflush = read_stat(arena_ind, "dalloc_noflush");
	size_t flush = read_stat(arena_ind, "dalloc_flush");
	size_t cached_bytes = read_stat(arena_ind, "bytes");

	expect_zu_gt(noflush, 1,
	    "most dallocs should be absorbed");
	expect_zu_gt(flush, 0,
	    "overflow should trigger at least one flush");
	expect_zu_gt(cached_bytes, 0,
	    "SEC should still hold extents after partial flush");
	expect_zu_le(cached_bytes, max_bytes,
	    "SEC should not exceed max_bytes");

	/*
	 * Step 5: Next alloc should be a SEC hit (cache is populated),
	 * and should not increase the miss counter.
	 */
	size_t misses_before = read_stat(arena_ind, "misses");
	void *p3 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p3, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "misses"), misses_before,
	    "alloc from populated SEC should not miss");
	dallocx(p3, flags);

	/*
	 * Step 6: Purge flushes SEC entirely.
	 */
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.purge", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "purge failed");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after purge");

	/*
	 * Step 7: Alloc after purge - must miss SEC again.
	 */
	size_t hits_before = read_stat(arena_ind, "hits");
	void *p4 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p4, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "hits"), hits_before,
	    "alloc after purge should miss SEC");
	dallocx(p4, flags);

	dallocx(ptrs, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_pac_sec_arena_reset) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");

	int    flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = sec_eligible_alloc_size();

	/* Populate SEC with a few cached extents. */
	void *p1 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p1, "mallocx failed");
	void *p2 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p2, "mallocx failed");
	dallocx(p1, flags);
	dallocx(p2, flags);
	expect_zu_gt(read_stat(arena_ind, "bytes"), 0,
	    "SEC should hold the freed extents");

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.reset", arena_ind);
	expect_d_eq(mallctl(buf, NULL, NULL, NULL, 0), 0,
	    "arena reset failed");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after arena reset");

	/* Arena is still usable: alloc and free again. */
	void *p3 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p3, "post-reset alloc failed");
	dallocx(p3, flags);

	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(buf, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pac_sec_arena_destroy) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");

	int    flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = sec_eligible_alloc_size();

	/* Populate SEC. */
	void *p1 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p1, "mallocx failed");
	dallocx(p1, flags);
	expect_zu_gt(read_stat(arena_ind, "bytes"), 0,
	    "SEC should hold the freed extent");

	/*
	 * Destroying the arena while SEC has cached extents must drain SEC
	 * cleanly (no leaks, no asserts).
	 */
	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(buf, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_pac_sec_alloc_dalloc_cycle,
	    test_pac_sec_arena_reset,
	    test_pac_sec_arena_destroy);
}
