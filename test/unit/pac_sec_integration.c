#include "test/jemalloc_test.h"

/*
 * Use multiple shards while keeping the single-threaded shard choice
 * deterministic.  Disable background threads to avoid decay races.
 */
const char *malloc_conf =
    "experimental_pac_sec_nshards:2,background_thread:false";

static sec_opts_t saved_pac_sec_opts;

static void
pac_sec_test_opts_set(void) {
	saved_pac_sec_opts = opt_pac_sec_opts;
	/*
	 * Include cache-oblivious padding so PAC can cache the test size.
	 */
	size_t test_extent_size = SC_LARGE_MINCLASS + sz_large_pad;
	opt_pac_sec_opts.max_alloc = test_extent_size;
	opt_pac_sec_opts.max_bytes = 4 * test_extent_size;
}

static void
pac_sec_test_opts_restore(void) {
	opt_pac_sec_opts = saved_pac_sec_opts;
}

static void *
pinned_extent_alloc(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind) {
	void *ret = ehooks_default_extent_hooks.alloc(
	    (extent_hooks_t *)&ehooks_default_extent_hooks, new_addr, size,
	    alignment, zero, commit, arena_ind);
	if (ret == NULL) {
		return NULL;
	}
	if (!*commit) {
		if (ehooks_default_extent_hooks.commit != NULL
		    && ehooks_default_extent_hooks.commit(
		        (extent_hooks_t *)&ehooks_default_extent_hooks, ret,
		        size, 0, size, arena_ind)) {
			ehooks_default_extent_hooks.dalloc(
			    (extent_hooks_t *)&ehooks_default_extent_hooks, ret,
			    size, *commit, arena_ind);
			return NULL;
		}
		*commit = true;
	}
	return (void *)((uintptr_t)ret | EXTENT_ALLOC_FLAG_PINNED);
}

static void
pinned_extent_destroy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	ehooks_default_extent_hooks.destroy(
	    (extent_hooks_t *)&ehooks_default_extent_hooks, addr, size,
	    committed, arena_ind);
}

static bool
pinned_extent_split(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	return ehooks_default_extent_hooks.split(
	    (extent_hooks_t *)&ehooks_default_extent_hooks, addr, size, size_a,
	    size_b, committed, arena_ind);
}

static bool
pinned_extent_merge(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	return ehooks_default_extent_hooks.merge(
	    (extent_hooks_t *)&ehooks_default_extent_hooks, addr_a, size_a,
	    addr_b, size_b, committed, arena_ind);
}

static extent_hooks_t pinned_hooks = {
	pinned_extent_alloc,
	NULL, /* dalloc */
	pinned_extent_destroy,
	NULL, /* commit */
	NULL, /* decommit */
	NULL, /* purge_lazy */
	NULL, /* purge_forced */
	pinned_extent_split,
	pinned_extent_merge
};

static size_t
read_arena_stat(unsigned arena_ind, const char *field) {
	char   cmd[128];
	size_t val;
	size_t sz = sizeof(val);
	uint64_t epoch = 1;
	sz = sizeof(epoch);
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sz), 0,
	    "Unexpected mallctl failure");
	sz = sizeof(val);
	snprintf(cmd, sizeof(cmd), "stats.arenas.%u.%s", arena_ind, field);
	expect_d_eq(mallctl(cmd, (void *)&val, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure reading arena stat");
	return val;
}

static size_t
read_stat(unsigned arena_ind, const char *field) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "pac_sec_%s", field);
	return read_arena_stat(arena_ind, cmd);
}

static size_t
read_extents_stat(unsigned arena_ind, pszind_t pszind, const char *field) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "extents.%u.%s", pszind, field);
	return read_arena_stat(arena_ind, cmd);
}

static size_t
read_pinned_npages(unsigned arena_ind) {
	tsd_t *tsd = tsd_fetch();
	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	expect_ptr_not_null(arena, "arena_get failed");
	return ecache_npages_get(&arena->pa_shard.pac.ecache_pinned);
}

static void
dirty_decay_ms_set(unsigned arena_ind, ssize_t decay_ms) {
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.dirty_decay_ms", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, (void *)&decay_ms,
	    sizeof(decay_ms)), 0, "dirty_decay_ms mallctl failed");
}

TEST_BEGIN(test_pac_sec_alloc_dalloc_cycle) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	pac_sec_test_opts_set();
	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");

	int    flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = SC_LARGE_MINCLASS;

	size_t max_bytes;
	sz = sizeof(max_bytes);
	expect_d_eq(mallctl("opt.experimental_pac_sec_max_bytes",
	    (void *)&max_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	size_t capacity = max_bytes / alloc_size;
	expect_zu_gt(capacity, 0, "SEC capacity must be > 0 for this test");

	/* Initial allocation misses SEC. */
	void *p1 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p1, "mallocx failed");
	size_t resident_after_alloc = read_arena_stat(arena_ind, "resident");
	expect_zu_eq(read_stat(arena_ind, "misses"), 1,
	    "first alloc should miss SEC");
	expect_zu_eq(read_stat(arena_ind, "hits"), 0,
	    "no hits yet");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty (extent is active)");

	/* Freeing the extent caches it in PAC SEC. */
	dallocx(p1, flags);
	size_t cached_after_one = read_stat(arena_ind, "bytes");
	expect_zu_gt(cached_after_one, 0,
	    "SEC should cache the freed extent");
	expect_zu_ge(read_arena_stat(arena_ind, "pdirty"),
	    cached_after_one >> LG_PAGE,
	    "SEC bytes should count toward dirty page stats");
	expect_zu_ge(read_arena_stat(arena_ind, "resident"),
	    resident_after_alloc,
	    "SEC bytes should remain included in resident stats");
	/* Use the actual extent size, including size class rounding. */
	size_t extent_size = cached_after_one;
	pszind_t extent_pszind = sz_psz2ind(extent_size);
	expect_zu_ge(read_extents_stat(arena_ind, extent_pszind, "ndirty"), 1,
	    "SEC extents should count toward extents dirty stats");
	expect_zu_ge(read_extents_stat(arena_ind, extent_pszind, "dirty_bytes"),
	    cached_after_one,
	    "SEC bytes should count toward extents dirty byte stats");
	expect_zu_eq(read_stat(arena_ind, "dalloc_noflush"), 1,
	    "one dalloc absorbed without flush");
	expect_zu_eq(read_stat(arena_ind, "dalloc_flush"), 0,
	    "no flush yet");

	/* Recompute capacity based on actual extent size. */
	capacity = max_bytes / extent_size;
	expect_zu_gt(capacity, 0, "SEC capacity should be positive");

	/* Reallocate from SEC. */
	void *p2 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p2, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "hits"), 1,
	    "second alloc should hit SEC");
	expect_zu_eq(read_stat(arena_ind, "misses"), 1,
	    "misses should not increase");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after hit");

	dallocx(p2, flags);

	/* Overflow SEC and flush cold extents to ecache_dirty. */
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

	/* A populated SEC serves the next allocation. */
	size_t misses_before = read_stat(arena_ind, "misses");
	void *p3 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p3, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "misses"), misses_before,
	    "alloc from populated SEC should not miss");
	dallocx(p3, flags);

	/* Purge flushes SEC entirely. */
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.purge", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "purge failed");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after purge");

	/* Allocation after purge misses SEC. */
	size_t hits_before = read_stat(arena_ind, "hits");
	void *p4 = mallocx(alloc_size, flags);
	expect_ptr_not_null(p4, "mallocx failed");
	expect_zu_eq(read_stat(arena_ind, "hits"), hits_before,
	    "alloc after purge should miss SEC");
	dallocx(p4, flags);

	dallocx(ptrs, MALLOCX_TCACHE_NONE);
	snprintf(cmd, sizeof(cmd), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
	pac_sec_test_opts_restore();
}
TEST_END

TEST_BEGIN(test_pac_sec_dirty_decay_toggle) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	pac_sec_test_opts_set();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected arenas.create failure");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = SC_LARGE_MINCLASS;

	void *p = mallocx(alloc_size, flags);
	expect_ptr_not_null(p, "mallocx failed");
	dallocx(p, flags);
	expect_zu_gt(read_stat(arena_ind, "bytes"), 0,
	    "SEC should cache when dirty decay is enabled");

	dirty_decay_ms_set(arena_ind, 0);
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "disabling dirty decay should flush SEC");

	p = mallocx(alloc_size, flags);
	expect_ptr_not_null(p, "mallocx failed");
	dallocx(p, flags);
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should stay disabled while dirty decay is zero");

	dirty_decay_ms_set(arena_ind, 100);
	p = mallocx(alloc_size, flags);
	expect_ptr_not_null(p, "mallocx failed");
	dallocx(p, flags);
	expect_zu_gt(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be usable after dirty decay is re-enabled");

	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
	pac_sec_test_opts_restore();
}
TEST_END

TEST_BEGIN(test_pac_sec_flush_pinned) {
	test_skip_if(!config_stats);
	test_skip_if(opt_hpa);

	pac_sec_test_opts_set();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)), 0,
	    "Unexpected arenas.create failure");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	size_t alloc_size = SC_LARGE_MINCLASS;
	size_t max_bytes;
	sz = sizeof(max_bytes);
	expect_d_eq(mallctl("opt.experimental_pac_sec_max_bytes",
	    (void *)&max_bytes, &sz, NULL, 0), 0,
	    "Unexpected mallctl failure");

	void *p = mallocx(alloc_size, flags);
	expect_ptr_not_null(p, "mallocx failed");
	size_t resident_after_alloc = read_arena_stat(arena_ind, "resident");
	dallocx(p, flags);
	size_t sec_bytes = read_stat(arena_ind, "bytes");
	expect_zu_gt(sec_bytes, 0, "SEC should cache the pinned extent");
	expect_zu_ge(read_arena_stat(arena_ind, "pinned"), sec_bytes,
	    "Pinned bytes cached in SEC should count toward pinned stats");
	expect_zu_ge(read_arena_stat(arena_ind, "resident"),
	    resident_after_alloc,
	    "Pinned SEC bytes should remain included in resident stats");

	size_t extent_size = sec_bytes;
	pszind_t extent_pszind = sz_psz2ind(extent_size);
	expect_zu_ge(read_extents_stat(arena_ind, extent_pszind, "npinned"), 1,
	    "Pinned SEC extents should count toward extents pinned stats");
	expect_zu_ge(read_extents_stat(arena_ind, extent_pszind, "pinned_bytes"),
	    sec_bytes,
	    "Pinned SEC bytes should count toward extents pinned byte stats");
	size_t nallocs = max_bytes / extent_size + 2;
	void **ptrs = mallocx(nallocs * sizeof(void *), MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(ptrs, "metadata alloc failed");
	for (size_t i = 0; i < nallocs; i++) {
		ptrs[i] = mallocx(alloc_size, flags);
		expect_ptr_not_null(ptrs[i], "mallocx %zu failed", i);
	}
	size_t pinned_before_overflow = read_pinned_npages(arena_ind);
	for (size_t i = 0; i < nallocs; i++) {
		dallocx(ptrs[i], flags);
	}
	expect_zu_gt(read_pinned_npages(arena_ind), pinned_before_overflow,
	    "SEC overflow should flush pinned extents to ecache_pinned");

	size_t pinned_before_purge = read_pinned_npages(arena_ind);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.purge", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "purge failed");
	expect_zu_eq(read_stat(arena_ind, "bytes"), 0,
	    "SEC should be empty after purge");
	expect_zu_gt(read_pinned_npages(arena_ind), pinned_before_purge,
	    "PAC SEC purge should flush pinned extents to ecache_pinned");

	dallocx(ptrs, MALLOCX_TCACHE_NONE);
	snprintf(cmd, sizeof(cmd), "arena.%u.destroy", arena_ind);
	expect_d_eq(mallctl(cmd, NULL, NULL, NULL, 0), 0,
	    "arena destroy failed");
	pac_sec_test_opts_restore();
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_pac_sec_alloc_dalloc_cycle, test_pac_sec_dirty_decay_toggle,
	    test_pac_sec_flush_pinned);
}
