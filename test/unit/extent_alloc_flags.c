#include "test/jemalloc_test.h"

static void *
pinned_extent_alloc(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind) {
	void *ret = ehooks_default_extent_hooks.alloc(
	    (extent_hooks_t *)&ehooks_default_extent_hooks,
	    new_addr, size, alignment, zero, commit, arena_ind);
	if (ret == NULL) {
		return NULL;
	}
	if (!*commit) {
		if (ehooks_default_extent_hooks.commit != NULL &&
		    ehooks_default_extent_hooks.commit(
		    (extent_hooks_t *)&ehooks_default_extent_hooks, ret, size,
		    0, size, arena_ind)) {
			ehooks_default_extent_hooks.dalloc(
			    (extent_hooks_t *)&ehooks_default_extent_hooks, ret,
			    size, *commit, arena_ind);
			return NULL;
		}
		*commit = true;
	}
	return (void *)((uintptr_t)ret | EXTENT_ALLOC_FLAG_PINNED);
}

static unsigned pinned_split_calls;
static unsigned pinned_destroy_calls;
static size_t   pinned_destroy_bytes;

static bool
pinned_extent_split(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	pinned_split_calls++;
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

static void
pinned_extent_destroy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	pinned_destroy_calls++;
	pinned_destroy_bytes += size;
	ehooks_default_extent_hooks.destroy(
	    (extent_hooks_t *)&ehooks_default_extent_hooks, addr, size,
	    committed, arena_ind);
}

static void
pinned_hooks_reset(void) {
	pinned_split_calls = 0;
	pinned_destroy_calls = 0;
	pinned_destroy_bytes = 0;
}

static extent_hooks_t pinned_hooks = {
	pinned_extent_alloc,
	NULL, /* dalloc — force retain */
	pinned_extent_destroy,
	NULL, /* commit */
	NULL, /* decommit */
	NULL, /* purge_lazy */
	NULL, /* purge_forced */
	pinned_extent_split,
	pinned_extent_merge
};

static size_t
get_arena_mapped(unsigned arena_ind) {
	uint64_t epoch = 1;
	size_t   epoch_sz = sizeof(epoch);
	expect_d_eq(0, mallctl("epoch", &epoch, &epoch_sz, &epoch,
	    sizeof(epoch)), "epoch failed");
	size_t mapped;
	size_t mapped_sz = sizeof(mapped);
	char   buf[64];
	snprintf(buf, sizeof(buf), "stats.arenas.%u.mapped", arena_ind);
	expect_d_eq(0, mallctl(buf, &mapped, &mapped_sz, NULL, 0),
	    "stats.arenas.<i>.mapped read failed");
	return mapped;
}

/*
 * Non-dependent emap lookup: returns the edata for addr, or NULL if the
 * rtree leaf does not exist (safe for addresses that jemalloc may never
 * have mapped, e.g. after arena destroy).
 */
static edata_t *
emap_edata_try_lookup(const void *ptr) {
	emap_full_alloc_ctx_t ctx;
	bool err = emap_full_alloc_ctx_try_lookup(TSDN_NULL,
	    &arena_emap_global, ptr, &ctx);
	if (err) {
		return NULL;
	}
	return ctx.edata;
}

/*
 * Find the edata covering addr by walking the emap from addr in PAGE
 * strides. Returns NULL if no covering edata is found within max_bytes.
 */
static edata_t *
find_covering_edata(const void *addr, size_t max_bytes) {
	uintptr_t a = (uintptr_t)addr;
	edata_t  *back = NULL;
	for (size_t off = 0; off <= max_bytes; off += PAGE) {
		back = emap_edata_try_lookup((void *)(a - off));
		if (back != NULL) {
			break;
		}
	}
	if (back == NULL) {
		return NULL;
	}
	edata_t *fwd = NULL;
	for (size_t off = 0; off <= max_bytes; off += PAGE) {
		fwd = emap_edata_try_lookup((void *)(a + off));
		if (fwd != NULL) {
			break;
		}
	}
	return (back == fwd) ? back : NULL;
}

TEST_BEGIN(test_pinned_stats) {
	test_skip_if(!config_stats);
	pinned_hooks_reset();

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	/* Create arena with pinned hooks. */
	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	/* Allocate and free to populate ecache_pinned. */
	void *p = mallocx(PAGE * 4, MALLOCX_ARENA(arena_ind)
	    | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "alloc failed");
	dallocx(p, MALLOCX_TCACHE_NONE);

	/* Refresh stats. */
	uint64_t epoch = 1;
	sz = sizeof(epoch);
	expect_d_eq(0, mallctl("epoch", &epoch, &sz, &epoch, sizeof(epoch)),
	    "epoch failed");

	/* Read total pinned stat. */
	char buf[128];
	size_t pinned_total;
	sz = sizeof(pinned_total);
	snprintf(buf, sizeof(buf), "stats.arenas.%u.pinned", arena_ind);
	expect_d_eq(0, mallctl(buf, &pinned_total, &sz, NULL, 0),
	    "stats.arenas.<i>.pinned read failed");
	expect_zu_gt(pinned_total, 0,
	    "pinned total should be > 0 after free to pinned arena");

	/* Pinned bytes are part of stats.mapped (unlike retained). */
	expect_zu_ge(get_arena_mapped(arena_ind), pinned_total,
	    "stats.mapped should include pinned bytes");

	/* Destroy the arena. */
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_shrink) {
	test_skip_if(ehooks_default_split_impl());
	pinned_hooks_reset();

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	/* Create arena with pinned hooks. */
	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	void *ptr = mallocx(SC_LARGE_MINCLASS + PAGE, flags);
	expect_ptr_not_null(ptr, "alloc failed");
	unsigned split_calls_before = pinned_split_calls;
	void *shrunk = rallocx(ptr, SC_LARGE_MINCLASS, flags);
	expect_ptr_not_null(shrunk, "shrink failed");
	expect_u_gt(pinned_split_calls, split_calls_before,
	    "shrink should invoke the split hook");
	dallocx(shrunk, MALLOCX_TCACHE_NONE);

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_remnant_lock) {
	test_skip_if(!opt_retain);
	pinned_hooks_reset();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	void *p1 = mallocx(SC_LARGE_MINCLASS, flags);
	expect_ptr_not_null(p1, "first alloc failed");

	tsd_t *tsd = tsd_fetch();
	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	expect_ptr_not_null(arena, "arena_get failed");
	expect_zu_gt(ecache_npages_get(&arena->pa_shard.pac.ecache_pinned),
	    0, "grow remnant should be cached in ecache_pinned");

	dallocx(p1, MALLOCX_TCACHE_NONE);

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_reuse) {
	pinned_hooks_reset();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	void *p1 = mallocx(PAGE * 2, flags);
	expect_ptr_not_null(p1, "first alloc failed");
	dallocx(p1, MALLOCX_TCACHE_NONE);

	void *p2 = mallocx(PAGE * 2, flags);
	expect_ptr_not_null(p2, "reuse alloc failed");
	expect_ptr_eq(p1, p2,
	    "pinned extent should be reused at the same address");
	dallocx(p2, MALLOCX_TCACHE_NONE);

	/* Destroy the arena. */
	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_realloc) {
	test_skip_if(ehooks_default_split_impl());
	pinned_hooks_reset();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/*
	 * Reallocs within the large-class range shrink/grow in place; the
	 * pointer stays at the original address rather than moving to a
	 * fresh extent (important since pinned memory is finite).  All sizes
	 * are multiples of SC_LARGE_MINCLASS so they bypass the slab path
	 * regardless of LG_PAGE / SC_LG_NGROUP.
	 */
	void *p = mallocx(SC_LARGE_MINCLASS * 7, flags);
	expect_ptr_not_null(p, "initial alloc failed");
	void *original = p;

	p = rallocx(p, SC_LARGE_MINCLASS * 3, flags);
	expect_ptr_not_null(p, "shrink failed");
	expect_ptr_eq(p, original, "shrink should preserve address");

	p = rallocx(p, SC_LARGE_MINCLASS * 6, flags);
	expect_ptr_not_null(p, "regrow failed");
	expect_ptr_eq(p, original, "regrow should preserve address");

	dallocx(p, MALLOCX_TCACHE_NONE);

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_reset) {
	pinned_hooks_reset();
	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;

	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Allocate several pinned extents, leave them live. */
	void *ptrs[4];
	for (int i = 0; i < 4; i++) {
		ptrs[i] = mallocx(SC_LARGE_MINCLASS * (i + 1), flags);
		expect_ptr_not_null(ptrs[i], "alloc %d failed", i);
	}

	/*
	 * arena.<i>.reset returns live allocations to the caches without
	 * destroying the arena.  No destroy hook calls should fire (pinned
	 * extents stay in ecache_pinned), and the arena should still be
	 * usable for further allocations afterward.
	 */
	unsigned destroy_calls_before = pinned_destroy_calls;
	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.reset", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena reset failed");
	expect_u_eq(pinned_destroy_calls, destroy_calls_before,
	    "reset should not invoke the destroy hook");

	tsd_t *tsd = tsd_fetch();
	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	expect_ptr_not_null(arena, "arena_get failed");
	expect_zu_gt(ecache_npages_get(&arena->pa_shard.pac.ecache_pinned), 0,
	    "pinned ecache should hold the reset extents");

	/* Arena is still usable: alloc and free again. */
	void *p = mallocx(SC_LARGE_MINCLASS, flags);
	expect_ptr_not_null(p, "post-reset alloc failed");
	dallocx(p, MALLOCX_TCACHE_NONE);

	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
}
TEST_END

TEST_BEGIN(test_pinned_destroy) {
	pinned_hooks_reset();

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	extent_hooks_t *hooks_ptr = &pinned_hooks;
	expect_d_eq(0, mallctl("arenas.create", &arena_ind, &sz,
	    &hooks_ptr, sizeof(hooks_ptr)),
	    "arena creation failed");

	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	size_t mapped_initial = config_stats ? get_arena_mapped(arena_ind) : 0;

	void *ptrs[4];
	size_t sizes[4] = {PAGE * 4, SC_LARGE_MINCLASS,
	    SC_LARGE_MINCLASS + PAGE, SC_LARGE_MINCLASS * 2};
	size_t total = 0;
	for (int i = 0; i < 4; i++) {
		ptrs[i] = mallocx(sizes[i], flags);
		expect_ptr_not_null(ptrs[i], "alloc %d failed", i);
		total += sizes[i];
	}

	if (config_stats) {
		expect_zu_ge(get_arena_mapped(arena_ind) - mapped_initial,
		    total,
		    "mapped should grow by at least total after pinned allocs");
	}

	/*
	 * Stress alloc/dalloc/realloc churn before the final teardown.  Mix
	 * sizes (some at the slab boundary, some multi-large-class), and on
	 * platforms that support splitting, alternate shrink and regrow on
	 * the same extent to stress the in-place realloc accounting paths.
	 */
	bool can_split = !ehooks_default_split_impl();
	for (int round = 0; round < 64; round++) {
		size_t s = SC_LARGE_MINCLASS + PAGE * (round % 4);
		void *p = mallocx(s, flags);
		expect_ptr_not_null(p, "churn alloc %d failed", round);
		if (can_split && (round & 1)) {
			void *shrunk = rallocx(p, SC_LARGE_MINCLASS, flags);
			expect_ptr_not_null(shrunk, "churn shrink %d failed",
			    round);
			void *regrown = rallocx(shrunk, SC_LARGE_MINCLASS * 3,
			    flags);
			expect_ptr_not_null(regrown, "churn regrow %d failed",
			    round);
			dallocx(regrown, MALLOCX_TCACHE_NONE);
		} else {
			void *p2 = rallocx(p, SC_LARGE_MINCLASS * 2, flags);
			expect_ptr_not_null(p2, "churn realloc %d failed",
			    round);
			dallocx(p2, MALLOCX_TCACHE_NONE);
		}
	}
	/* Free and re-alloc one of the original pointers to exercise reuse. */
	dallocx(ptrs[1], MALLOCX_TCACHE_NONE);
	ptrs[1] = mallocx(sizes[1], flags);
	expect_ptr_not_null(ptrs[1], "reuse alloc failed");

	for (int i = 0; i < 4; i++) {
		dallocx(ptrs[i], MALLOCX_TCACHE_NONE);
	}

	if (config_stats) {
		expect_zu_ge(get_arena_mapped(arena_ind) - mapped_initial,
		    total,
		    "pinned bytes should remain in mapped after dalloc "
		    "(no decay for pinned)");
	}

	tsd_t *tsd = tsd_fetch();
	tsdn_t *tsdn = tsd_tsdn(tsd);
	arena_t *arena = arena_get(tsdn, arena_ind, false);
	expect_ptr_not_null(arena, "arena_get failed");
	size_t pinned_bytes =
	    ecache_npages_get(&arena->pa_shard.pac.ecache_pinned) << LG_PAGE;
	expect_zu_gt(pinned_bytes, 0,
	    "pinned ecache should contain the freed extents");

	/*
	 * Pinned extents stay registered in the emap after dalloc.  Coalescing
	 * may have made any individual ptrs[i] interior to a larger merged
	 * extent, so use find_covering_edata to walk to the merged extent's
	 * base/last-page; each must resolve to a pinned-state edata.  The
	 * search bound is the total bytes currently in ecache_pinned, which
	 * upper-bounds the size of any covering merged extent.
	 */
	for (int i = 0; i < 4; i++) {
		edata_t *covering = find_covering_edata(ptrs[i],
		    pinned_bytes);
		expect_ptr_not_null(covering,
		    "freed pinned extent ptrs[%d] should still be reachable "
		    "in the emap", i);
		expect_d_eq(edata_state_get(covering), extent_state_pinned,
		    "covering extent for ptrs[%d] should be in pinned state",
		    i);
	}

	char buf[64];
	snprintf(buf, sizeof(buf), "arena.%u.destroy", arena_ind);
	unsigned destroy_calls_before = pinned_destroy_calls;
	size_t destroy_bytes_before = pinned_destroy_bytes;
	expect_d_eq(0, mallctl(buf, NULL, NULL, NULL, 0),
	    "arena destroy failed");
	expect_u_gt(pinned_destroy_calls, destroy_calls_before,
	    "arena destroy should invoke the destroy hook");
	expect_zu_ge(pinned_destroy_bytes - destroy_bytes_before, total,
	    "destroy hook should be called with at least the allocated total "
	    "(coalesced fragments returned to OS)");
	if (maps_coalesce) {
		expect_u_lt(pinned_destroy_calls - destroy_calls_before, 4,
		    "destroy calls should be < #allocs after coalesce");
	}
	for (int i = 0; i < 4; i++) {
		expect_ptr_null(find_covering_edata(ptrs[i], pinned_bytes),
		    "arena destroy should clear the emap entry for ptrs[%d]",
		    i);
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_pinned_remnant_lock,
	    test_pinned_reuse,
	    test_pinned_realloc,
	    test_pinned_stats,
	    test_pinned_shrink,
	    test_pinned_reset,
	    test_pinned_destroy);
}
