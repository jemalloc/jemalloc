#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa_shard_set.h"

/*
 * Except where noted, these tests exercise the routing logic in isolation: no
 * shards are constructed, so no hugepages are mapped.  They build the shard
 * sets by hand, fill in only the fields the router reads, and check the two
 * decisions the design makes: 1. size to shard set and 2. shard set to shard.
 */
#define SET_SMALL_MAX (4 * PAGE)  /* 16 KiB with the default page size. */
#define SET_MED_MAX   (16 * PAGE) /* 64 KiB with the default page size. */

/* Real objects used only to give the test slots distinct shard pointers. */
#define TEST_MAX_SHARDS 16
static hpa_shard_t test_shards[TEST_MAX_SHARDS];
/* Slot storage for hand-built sets, carved up in shard_sets_init(). */
static atomic_p_t test_slots[TEST_MAX_SHARDS];

static void
shard_sets_init(hpa_shard_sets_t *sets, unsigned nsets, const size_t *bounds,
    const unsigned *nshards, hpa_shard_set_pick_t pick) {
	memset(sets, 0, sizeof(*sets));
	sets->nsets = nsets;
	unsigned next_shard = 0;
	for (unsigned i = 0; i < nsets; i++) {
		assert_u_le(next_shard + nshards[i], TEST_MAX_SHARDS,
		    "test configuration too large");
		sets->sets[i].size_max = bounds[i];
		sets->sets[i].nshards = nshards[i];
		sets->sets[i].pick = pick;
		sets->sets[i].shards = &test_slots[next_shard];
		atomic_store_u(&sets->sets[i].rr_next, 0, ATOMIC_RELAXED);
		for (unsigned j = 0; j < nshards[i]; j++) {
			atomic_store_p(&sets->sets[i].shards[j],
			    &test_shards[next_shard + j], ATOMIC_RELAXED);
		}
		next_shard += nshards[i];
	}
	sets->initialized = true;
}

static unsigned
route_to_test_shard(
    hpa_shard_sets_t *sets, size_t size, unsigned affinity) {
	hpa_shard_t *shard = hpa_route(
	    TSDN_NULL, sets, sz_s2u(size), affinity);
	assert_ptr_not_null(shard, "size %zu routed to no shard", size);
	return (unsigned)(shard - test_shards);
}

/*
 * Checks that lookup returns, for every usable size up to the last set's
 * size_max, the first set whose size_max is at least that size, and that
 * lookup and routing return NULL for the next usable size above it.
 */
static void
expect_route_coverage(hpa_shard_sets_t *sets) {
	size_t last = sets->sets[sets->nsets - 1].size_max;
	for (size_t usize = sz_s2u(1); usize <= last;
	    usize = sz_s2u(usize + 1)) {
		hpa_shard_set_t *shard_set = hpa_shard_sets_lookup(sets, usize);
		expect_ptr_not_null(shard_set, "size %zu routed nowhere", usize);
		if (shard_set == NULL) {
			continue;
		}
		size_t size_min = shard_set == &sets->sets[0]
		    ? 1 : (shard_set - 1)->size_max + 1;
		expect_true(size_min <= usize && usize <= shard_set->size_max,
		    "size %zu routed to the set covering [%zu, %zu]", usize,
		    size_min, shard_set->size_max);
	}

	size_t above = sz_s2u(last + 1);
	expect_ptr_null(hpa_shard_sets_lookup(sets, above),
	    "size %zu above the last bound should have no shard set", above);
	expect_ptr_null(hpa_route(TSDN_NULL, sets, above, 0),
	    "size %zu above the last bound should route to no shard", above);
}

TEST_BEGIN(test_route_coverage) {
	/*
	 * Sizes up to the last bound route to the set covering them, and sizes
	 * above it route nowhere, so the caller falls back to the PAC.
	 */
	hpa_shard_sets_t sets;
	unsigned         nshards[] = {4, 2, 1};
	size_t           bounds[] = {SET_SMALL_MAX, SET_MED_MAX, HUGEPAGE};
	shard_sets_init(&sets, 3, bounds, nshards, hpa_shard_set_pick_arena);
	expect_route_coverage(&sets);

	/* The last set may end below HUGEPAGE. */
	shard_sets_init(&sets, 2, bounds, nshards, hpa_shard_set_pick_arena);
	expect_route_coverage(&sets);

	/*
	 * With large size classes disabled, large usable sizes are page
	 * multiples, so a bound may fall inside a size class and must still be
	 * honored exactly.
	 */
	if (sz_large_size_classes_disabled()) {
		size_t granular[] = {SET_SMALL_MAX, 18 * PAGE, HUGEPAGE};
		assert_zu_eq(sz_s2u(granular[1]), granular[1],
		    "the bound must be a usable size");
		shard_sets_init(
		    &sets, 3, granular, nshards, hpa_shard_set_pick_arena);
		expect_route_coverage(&sets);
	}
}
TEST_END

/* Real shard sets with their own base, for tests that create shards. */
typedef struct real_sets_s {
	base_t          *base;
	edata_cache_t    edata_cache;
	emap_t           emap;
	hpa_central_t    central;
	hpa_shard_sets_t sets;
} real_sets_t;

/* Boots two sets of four slots each; all slots start empty. */
static void
real_sets_boot(tsdn_t *tsdn, real_sets_t *rs) {
	rs->base = base_new(tsdn, /* ind */ 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	assert_ptr_not_null(rs->base, "");
	assert_false(edata_cache_init(&rs->edata_cache, rs->base), "");
	assert_false(emap_init(&rs->emap, rs->base, /* zeroed */ false), "");
	assert_false(
	    hpa_central_init(&rs->central, rs->base, &hpa_hooks_default), "");

	hpa_shard_sets_config_t config;
	memset(&config, 0, sizeof(config));
	config.nsets = 2;
	config.sets[0].size_max = SET_SMALL_MAX;
	config.sets[0].nshards = 4;
	config.sets[1].size_max = HUGEPAGE;
	config.sets[1].nshards = 4;

	memset(&rs->sets, 0, sizeof(rs->sets));
	assert_false(hpa_shard_sets_boot(tsdn, &rs->sets, rs->base,
	    &rs->central, &rs->emap, &rs->edata_cache, &config, &opt_hpa_opts,
	    &opt_hpa_sec_opts), "booting the shard sets failed");
}

TEST_BEGIN(test_lazy_init_uses_slot_as_owner_id) {
	test_skip_if(!hpa_supported());

	/*
	 * Unlike the tests above, this constructs real shards: initializing the
	 * same slot in two sets must give both shards that slot as their owner
	 * id.
	 */
	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	real_sets_t rs;
	real_sets_boot(tsdn, &rs);
	hpa_shard_sets_t *sets = &rs.sets;
	for (unsigned i = 0; i < 2; i++) {
		for (unsigned j = 0; j < sets->sets[i].nshards; j++) {
			expect_ptr_null(hpa_shard_set_shard_from_id(
			    &sets->sets[i], j),
			    "set %u: slot %u should start empty", i, j);
		}
	}

	unsigned     slot = 2;
	hpa_shard_t *shards[2];
	for (unsigned i = 0; i < 2; i++) {
		shards[i] = hpa_shard_set_pick_shard(tsdn, &sets->sets[i], slot);
		assert_ptr_not_null(shards[i], "set %u: lazy init failed", i);
		expect_u_eq(shards[i]->ind, slot,
		    "set %u: the owner id should be the slot index", i);
		expect_ptr_eq(hpa_shard_set_shard_from_id(&sets->sets[i], slot),
		    shards[i], "set %u: the owner id should resolve", i);
	}
	expect_ptr_ne(shards[0], shards[1], "each set should own its shards");

	base_delete(tsdn, rs.base);
}
TEST_END

TEST_BEGIN(test_fork_hooks) {
	test_skip_if(!hpa_supported());

	/*
	 * The fork hooks must lock init_mtx before any shard lock (witness
	 * checks the rank order in debug builds) and must reach every
	 * initialized shard, but skip empty slots.
	 */
	tsd_t      *tsd = tsd_fetch();
	tsdn_t     *tsdn = tsd_tsdn(tsd);
	real_sets_t rs;
	real_sets_boot(tsdn, &rs);
	hpa_shard_sets_t *sets = &rs.sets;

	/* Leave some slots empty: slots 0 and 3 in set 0, slot 1 in set 1. */
	hpa_shard_t *live[3];
	live[0] = hpa_shard_set_pick_shard(tsdn, &sets->sets[0], 0);
	live[1] = hpa_shard_set_pick_shard(tsdn, &sets->sets[0], 3);
	live[2] = hpa_shard_set_pick_shard(tsdn, &sets->sets[1], 1);
	for (unsigned k = 0; k < 3; k++) {
		assert_ptr_not_null(live[k], "lazy init failed");
	}

	/* As _malloc_prefork() does, relax same-rank checks while forking. */
	witness_prefork(tsd_witness_tsdp_get(tsd));
	hpa_shard_sets_prefork2(tsdn, sets);
	hpa_shard_sets_prefork3(tsdn, sets);
	hpa_shard_sets_prefork4(tsdn, sets);

	for (unsigned i = 0; i < sets->nsets; i++) {
		malloc_mutex_assert_owner(tsdn, &sets->sets[i].init_mtx);
	}
	for (unsigned k = 0; k < 3; k++) {
		malloc_mutex_assert_owner(tsdn, &live[k]->mtx);
		malloc_mutex_assert_owner(tsdn, &live[k]->grow_mtx);
		if (sec_is_used(&live[k]->sec)) {
			malloc_mutex_assert_owner(
			    tsdn, &live[k]->sec.bins[0].mtx);
		}
	}

	hpa_shard_sets_postfork_parent(tsdn, sets);
	witness_postfork_parent(tsd_witness_tsdp_get(tsd));

	/* Everything is released: lazy init and lookup still work. */
	expect_ptr_eq(hpa_shard_set_pick_shard(tsdn, &sets->sets[0], 0),
	    live[0], "an existing shard should still be returned");
	hpa_shard_t *fresh = hpa_shard_set_pick_shard(tsdn, &sets->sets[1], 2);
	expect_ptr_not_null(fresh, "lazy init should work after the hooks");
	expect_ptr_eq(hpa_shard_set_shard_from_id(&sets->sets[1], 2), fresh,
	    "the new shard should be published");

	base_delete(tsdn, rs.base);
}
TEST_END

TEST_BEGIN(test_pick_arena_uses_affinity) {
	/*
	 * One set spanning all sizes with one shard per arena and the arena
	 * picker reproduces the per-arena layout: arena i always gets shard i.
	 */
	hpa_shard_sets_t sets;
	unsigned         narenas = 10;
	size_t           bounds[] = {HUGEPAGE};
	unsigned         nshards[] = {narenas};
	shard_sets_init(&sets, 1, bounds, nshards, hpa_shard_set_pick_arena);

	for (unsigned arena = 0; arena < narenas; arena++) {
		for (size_t size = 8; size <= HUGEPAGE; size *= 2) {
			expect_u_eq(route_to_test_shard(&sets, size, arena),
			    arena, "arena %u, size %zu: the arena picker must "
			    "route to shard %u", arena, size, arena);
		}
	}
}
TEST_END

TEST_BEGIN(test_pick_roundrobin_spreads) {
	/* Round robin should ignore the affinity and cycle through slots. */
	hpa_shard_sets_t sets;
	size_t           bounds[] = {HUGEPAGE};
	unsigned         nshards[] = {4};
	shard_sets_init(
	    &sets, 1, bounds, nshards, hpa_shard_set_pick_roundrobin);

	unsigned counts[4] = {0};
	for (unsigned i = 0; i < 4 * 25; i++) {
		/* Constant affinity: any spreading is the picker's doing. */
		unsigned ind = route_to_test_shard(
		    &sets, PAGE, /* affinity */ 7);
		expect_u_lt(ind, 4, "shard index out of range");
		counts[ind]++;
	}
	for (unsigned i = 0; i < 4; i++) {
		expect_u_eq(counts[i], 25,
		    "round robin should have hit shard %u exactly 25 times",
		    i);
	}
}
TEST_END

TEST_BEGIN(test_config_validate) {
	hpa_shard_sets_config_t config;
	memset(&config, 0, sizeof(config));
	config.nsets = 2;
	config.sets[0].size_max = SET_SMALL_MAX;
	config.sets[0].nshards = 2;
	config.sets[1].size_max = HUGEPAGE;
	config.sets[1].nshards = 1;
	config.sets[1].pick = hpa_shard_set_pick_roundrobin;
	expect_false(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "valid configuration rejected");

	config.sets[0].size_max++;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "bound that is not a usable size accepted");
	config.sets[0].size_max = SET_SMALL_MAX;
	config.sets[1].nshards = 0;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "set without shards accepted");
	config.sets[1].nshards = HPA_MAX_SHARDS_PER_SET + 1;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "set with more shards than slots accepted");
	config.sets[1].nshards = 1;

	config.nsets = 0;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "configuration without sets accepted");
	config.nsets = HPA_MAX_SHARD_SETS + 1;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "configuration with too many sets accepted");
	config.nsets = 2;
	/* Each set's picker is checked, not just the first. */
	config.sets[1].pick = hpa_shard_set_pick_limit;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "unknown picker accepted");
	config.sets[1].pick = hpa_shard_set_pick_roundrobin;

	config.sets[1].size_max = SET_SMALL_MAX;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "bound not above the previous bound accepted");
	config.sets[1].size_max = 2 * HUGEPAGE;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "bound above HUGEPAGE accepted");
	config.sets[1].size_max = HUGEPAGE;
	expect_false(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "valid configuration rejected after the checks above");

	/* The sets must cover every size the HPA serves, and may cover more. */
	config.sets[1].size_max = SET_MED_MAX;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "configuration short of slab_max_alloc accepted");
	expect_false(hpa_shard_sets_config_validate(&config, SET_MED_MAX),
	    "configuration covering exactly slab_max_alloc rejected");
	expect_false(hpa_shard_sets_config_validate(&config, PAGE),
	    "configuration beyond slab_max_alloc rejected");

	/* Small sizes are always served, whatever slab_max_alloc is. */
	config.nsets = 1;
	config.sets[0].size_max = SC_LARGE_MINCLASS;
	expect_false(hpa_shard_sets_config_validate(&config, PAGE),
	    "configuration covering all small sizes rejected");
	config.sets[0].size_max = SC_SMALL_MAXCLASS;
	expect_true(hpa_shard_sets_config_validate(&config, PAGE),
	    "configuration short of the small/large boundary accepted");
}
TEST_END

TEST_BEGIN(test_config_add) {
	hpa_shard_sets_config_t config;
	memset(&config, 0, sizeof(config));
	hpa_shard_set_pick_t arena = hpa_shard_set_pick_arena;
	hpa_shard_set_pick_t roundrobin = hpa_shard_set_pick_roundrobin;

	expect_true(hpa_shard_sets_config_add(
	    &config, 2, SET_SMALL_MAX, 1, arena),
	    "the first set must start at 1");
	expect_false(hpa_shard_sets_config_add(
	    &config, 1, SET_SMALL_MAX, 4, arena),
	    "a set starting at 1 rejected");
	expect_true(hpa_shard_sets_config_add(
	    &config, SET_SMALL_MAX + 2, HUGEPAGE, 1, arena),
	    "a gap between sets accepted");
	expect_true(hpa_shard_sets_config_add(
	    &config, SET_SMALL_MAX + 1, SET_SMALL_MAX, 1, arena),
	    "an empty range accepted");
	expect_false(hpa_shard_sets_config_add(
	    &config, SET_SMALL_MAX + 1, HUGEPAGE, 2, roundrobin),
	    "a contiguous set rejected");
	expect_u_eq(config.nsets, 2, "rejected sets should not be added");
	expect_zu_eq(config.sets[1].size_max, HUGEPAGE, "");
	expect_u_eq(config.sets[1].nshards, 2, "");
	expect_u_eq(config.sets[0].pick, arena, "");
	expect_u_eq(config.sets[1].pick, roundrobin, "");

	/* The container holds at most HPA_MAX_SHARD_SETS sets. */
	memset(&config, 0, sizeof(config));
	for (unsigned i = 0; i < HPA_MAX_SHARD_SETS; i++) {
		expect_false(hpa_shard_sets_config_add(
		    &config, i * PAGE + 1, (i + 1) * PAGE, 1, arena), "");
	}
	expect_true(hpa_shard_sets_config_add(&config,
	    HPA_MAX_SHARD_SETS * PAGE + 1, HUGEPAGE, 1, arena),
	    "a set past HPA_MAX_SHARD_SETS accepted");
	expect_u_eq(config.nsets, HPA_MAX_SHARD_SETS, "");
}
TEST_END

TEST_BEGIN(test_pick_from_name) {
	hpa_shard_set_pick_t pick;
	expect_false(hpa_shard_set_pick_from_name("arena", 5, &pick), "");
	expect_u_eq(pick, hpa_shard_set_pick_arena, "");
	expect_false(
	    hpa_shard_set_pick_from_name("roundrobin|", 10, &pick),
	    "only the given length should be compared");
	expect_u_eq(pick, hpa_shard_set_pick_roundrobin, "");
	expect_true(hpa_shard_set_pick_from_name("a", 1, &pick),
	    "a prefix of a picker name accepted");
	expect_true(hpa_shard_set_pick_from_name("arenas", 6, &pick),
	    "a name longer than any picker accepted");
	expect_true(hpa_shard_set_pick_from_name("", 0, &pick),
	    "an empty name accepted");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_route_coverage,
	    test_lazy_init_uses_slot_as_owner_id, test_fork_hooks,
	    test_pick_arena_uses_affinity, test_pick_roundrobin_spreads,
	    test_config_validate, test_config_add, test_pick_from_name);
}
