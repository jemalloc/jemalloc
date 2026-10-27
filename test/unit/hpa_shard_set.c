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

/*
 * Allocates and boots two sets of four slots each, with the given shard
 * options; all slots start empty.
 */
static real_sets_t *
real_sets_create_opts(tsdn_t *tsdn, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts) {
	/* emap's radix-tree root is too large for the default Windows stack. */
	real_sets_t *rs = calloc(1, sizeof(*rs));
	assert_ptr_not_null(rs, "");

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
	    &rs->central, &rs->emap, &rs->edata_cache, &config, opts, sec_opts),
	    "booting the shard sets failed");
	return rs;
}

/* Allocates and boots the sets with the runtime shard options. */
static real_sets_t *
real_sets_create(tsdn_t *tsdn) {
	return real_sets_create_opts(tsdn, &opt_hpa_opts, &opt_hpa_sec_opts);
}

static void
real_sets_destroy(tsdn_t *tsdn, real_sets_t *rs) {
	base_delete(tsdn, rs->base);
	free(rs);
}

TEST_BEGIN(test_lazy_init_uses_slot_as_owner_id) {
	test_skip_if(!hpa_supported());

	/*
	 * Unlike the tests above, this constructs real shards: initializing the
	 * same slot in two sets must give both shards that slot as their owner
	 * id.
	 */
	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	real_sets_t *rs = real_sets_create(tsdn);
	hpa_shard_sets_t *sets = &rs->sets;
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

	real_sets_destroy(tsdn, rs);
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
	real_sets_t *rs = real_sets_create(tsdn);
	hpa_shard_sets_t *sets = &rs->sets;

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

	real_sets_destroy(tsdn, rs);
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

/*
 * The statistics tests use three shards placed to catch a walk that stops
 * early or skips slots: the last slot of set 0, behind three empty slots, and
 * two shards of set 1 with an empty slot between them.
 *
 * Their checks avoid values that depend on HPA or SEC policy (how many extents
 * a SEC fill takes, when pages are hugified, which bucket a pageslab is in).
 * They compare the walks with the shards' own counters, and use identities
 * that hold whatever that policy is.
 */
#define STATS_NSHARDS 3
static const unsigned stats_set_ind[STATS_NSHARDS] = {0, 1, 1};
static const unsigned stats_slot[STATS_NSHARDS] = {3, 0, 2};

typedef struct stats_env_s {
	real_sets_t *rs;
	hpa_shard_t *shards[STATS_NSHARDS];
	/* One page per shard, allocated until stats_env_fini(). */
	edata_t *edatas[STATS_NSHARDS];
} stats_env_t;

static edata_t *
stats_alloc(tsdn_t *tsdn, hpa_shard_t *shard, size_t size) {
	bool     deferred_work_generated = false;
	edata_t *edata = hpa_alloc(tsdn, shard, size, PAGE, /* zero */ false,
	    /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	assert_ptr_not_null(edata, "hpa_alloc(%zu) failed", size);
	return edata;
}

static void
stats_dalloc(tsdn_t *tsdn, hpa_shard_t *shard, edata_t *edata) {
	bool deferred_work_generated = false;
	hpa_dalloc(tsdn, shard, edata, &deferred_work_generated);
}

/*
 * Boots the sets with purging off, so freed pages stay dirty, and with every
 * size up to a hugepage served by the shards.  Each shard keeps one page
 * allocated, which the SEC may fill extra pages for, and frees one extent too
 * large for the SEC, which leaves dirty pages.
 */
static void
stats_env_init(tsdn_t *tsdn, stats_env_t *env) {
	hpa_shard_opts_t opts = opt_hpa_opts;
	opts.dirty_mult = (fxp_t)-1;
	opts.slab_max_alloc = HUGEPAGE;
	env->rs = real_sets_create_opts(tsdn, &opts, &opt_hpa_sec_opts);

	for (unsigned i = 0; i < STATS_NSHARDS; i++) {
		hpa_shard_set_t *shard_set =
		    &env->rs->sets.sets[stats_set_ind[i]];
		env->shards[i] = hpa_shard_set_pick_shard(
		    tsdn, shard_set, stats_slot[i]);
		assert_ptr_not_null(env->shards[i], "lazy init failed");
		assert_ptr_eq(
		    hpa_shard_set_shard_from_id(shard_set, stats_slot[i]),
		    env->shards[i], "the shard should be in its slot");
		env->edatas[i] = stats_alloc(tsdn, env->shards[i], PAGE);

		/* The smallest page multiple the SEC does not cache. */
		size_t big = PAGE;
		while (sec_size_supported(&env->shards[i]->sec, big)) {
			big += PAGE;
		}
		assert_zu_le(big, HUGEPAGE / 2,
		    "each shard's extents should fit in one pageslab");
		stats_dalloc(tsdn, env->shards[i],
		    stats_alloc(tsdn, env->shards[i], big));
	}
}

static void
stats_env_fini(tsdn_t *tsdn, stats_env_t *env) {
	for (unsigned i = 0; i < STATS_NSHARDS; i++) {
		stats_dalloc(tsdn, env->shards[i], env->edatas[i]);
	}
	real_sets_destroy(tsdn, env->rs);
}

TEST_BEGIN(test_stats_merge_walk) {
	test_skip_if(!hpa_supported());

	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	stats_env_t env;
	stats_env_init(tsdn, &env);
	hpa_shard_sets_t *sets = &env.rs->sets;

	/* The walk merges every initialized shard exactly once. */
	hpa_shard_stats_t walk, oracle;
	memset(&walk, 0, sizeof(walk));
	memset(&oracle, 0, sizeof(oracle));
	hpa_shard_sets_stats_merge(tsdn, sets, &walk);
	for (unsigned i = 0; i < STATS_NSHARDS; i++) {
		hpa_shard_stats_merge(tsdn, env.shards[i], &oracle);
	}
	expect_d_eq(memcmp(&walk, &oracle, sizeof(walk)), 0,
	    "the walk should equal the sum of the shards' own statistics");
	expect_zu_eq(walk.psset_stats.merged.npageslabs, STATS_NSHARDS,
	    "each shard should contribute its one pageslab");

	/*
	 * Every active page is either handed out, one per shard, or cached in
	 * a shard's SEC, however many pages a SEC fill took.
	 */
	expect_zu_eq(walk.psset_stats.merged.nactive,
	    STATS_NSHARDS + (walk.secstats.bytes >> LG_PAGE),
	    "active pages should be the handed-out pages plus the SEC's");

	/* The lock-free dirty walk counts the same pages as the merge. */
	size_t ndirty = hpa_shard_sets_ndirty(sets);
	assert_zu_ne(
	    ndirty, 0, "the freed extents should have left dirty pages");
	expect_zu_eq(ndirty, walk.psset_stats.merged.ndirty,
	    "the dirty walk and the merge should agree");

	/* The walk adds to its output rather than overwriting it. */
	hpa_shard_stats_t twice, doubled;
	memset(&twice, 0, sizeof(twice));
	hpa_shard_sets_stats_merge(tsdn, sets, &twice);
	hpa_shard_sets_stats_merge(tsdn, sets, &twice);
	doubled = walk;
	hpa_shard_stats_accum(&doubled, &walk);
	expect_d_eq(memcmp(&twice, &doubled, sizeof(twice)), 0,
	    "merging twice should double every statistic");

	stats_env_fini(tsdn, &env);
}
TEST_END

/* Reads the sets' mutex counters (mtx, grow_mtx, SEC bins) into zeroes. */
static void
stats_mtx_read(tsdn_t *tsdn, hpa_shard_sets_t *sets, mutex_prof_data_t *data) {
	memset(data, 0, 3 * sizeof(*data));
	hpa_shard_sets_mtx_stats_read(tsdn, sets, &data[0], &data[1], &data[2]);
}

/* A mutex's lock count, including the lock taken to read it. */
static uint64_t
stats_mtx_lock_ops(tsdn_t *tsdn, malloc_mutex_t *mtx) {
	mutex_prof_data_t data;
	malloc_mutex_lock(tsdn, mtx);
	malloc_mutex_prof_read(tsdn, &data, mtx);
	malloc_mutex_unlock(tsdn, mtx);
	return data.n_lock_ops;
}

TEST_BEGIN(test_mtx_stats_walk) {
	test_skip_if(!hpa_supported());
	test_skip_if(!config_stats);

	static const char *const names[3] = {"mtx", "grow_mtx", "SEC bins"};
	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	stats_env_t env;
	stats_env_init(tsdn, &env);
	hpa_shard_sets_t *sets = &env.rs->sets;

	/*
	 * A read locks each mutex it reads once.  The difference between two
	 * reads gives that count without depending on how many bins a SEC has.
	 */
	mutex_prof_data_t r1[3], r2[3];
	stats_mtx_read(tsdn, sets, r1);
	stats_mtx_read(tsdn, sets, r2);
	uint64_t per_read[3];
	for (unsigned k = 0; k < 3; k++) {
		per_read[k] = r2[k].n_lock_ops - r1[k].n_lock_ops;
	}
	expect_u64_eq(per_read[0], STATS_NSHARDS,
	    "a read should lock each shard's mtx once");
	expect_u64_eq(per_read[1], STATS_NSHARDS,
	    "a read should lock each shard's grow_mtx once");

	/* A read sums the shards' own counters. */
	uint64_t oracle[3] = {0, 0, 0};
	for (unsigned i = 0; i < STATS_NSHARDS; i++) {
		hpa_shard_t *shard = env.shards[i];
		oracle[0] += stats_mtx_lock_ops(tsdn, &shard->mtx);
		oracle[1] += stats_mtx_lock_ops(tsdn, &shard->grow_mtx);
		mutex_prof_data_t sec_data;
		memset(&sec_data, 0, sizeof(sec_data));
		sec_mutex_stats_read(tsdn, &shard->sec, &sec_data);
		oracle[2] += sec_data.n_lock_ops;
	}
	mutex_prof_data_t r3[3];
	stats_mtx_read(tsdn, sets, r3);
	for (unsigned k = 0; k < 3; k++) {
		expect_u64_eq(r3[k].n_lock_ops, oracle[k] + per_read[k],
		    "%s: the read should sum the shards' counters", names[k]);
	}

	/*
	 * A read accumulates into its outputs: counters add and maxima stay
	 * maxima.  The test is single-threaded, so no lock waits between reads.
	 */
	mutex_prof_data_t pre;
	memset(&pre, 0, sizeof(pre));
	pre.n_lock_ops = 1000;
	pre.n_wait_times = 100;
	pre.n_spin_acquired = 10;
	pre.max_n_thds = 1000;
	nstime_init(&pre.tot_wait_time, 123456);
	nstime_init(&pre.max_wait_time, 1000 * 1000 * 1000);
	mutex_prof_data_t r4[3] = {pre, pre, pre};
	hpa_shard_sets_mtx_stats_read(tsdn, sets, &r4[0], &r4[1], &r4[2]);
	for (unsigned k = 0; k < 3; k++) {
		expect_u64_eq(r4[k].n_lock_ops,
		    pre.n_lock_ops + r3[k].n_lock_ops + per_read[k],
		    "%s: lock counts should add", names[k]);
		expect_u64_eq(r4[k].n_wait_times,
		    pre.n_wait_times + r3[k].n_wait_times,
		    "%s: wait counts should add", names[k]);
		expect_u64_eq(r4[k].n_spin_acquired,
		    pre.n_spin_acquired + r3[k].n_spin_acquired,
		    "%s: spin counts should add", names[k]);
		expect_u64_eq(nstime_ns(&r4[k].tot_wait_time),
		    nstime_ns(&pre.tot_wait_time)
		        + nstime_ns(&r3[k].tot_wait_time),
		    "%s: total wait times should add", names[k]);
		expect_u32_eq(r4[k].max_n_thds,
		    pre.max_n_thds > r3[k].max_n_thds ? pre.max_n_thds
		                                      : r3[k].max_n_thds,
		    "%s: the maximum thread count should be the larger",
		    names[k]);
		uint64_t pre_max = nstime_ns(&pre.max_wait_time);
		uint64_t r3_max = nstime_ns(&r3[k].max_wait_time);
		expect_u64_eq(nstime_ns(&r4[k].max_wait_time),
		    pre_max > r3_max ? pre_max : r3_max,
		    "%s: the maximum wait time should be the larger", names[k]);
	}

	/*
	 * A reset clears every counter, so a read then counts only its own
	 * locks, and leaves the shards' statistics alone.
	 */
	hpa_shard_stats_t before, after;
	memset(&before, 0, sizeof(before));
	memset(&after, 0, sizeof(after));
	hpa_shard_sets_stats_merge(tsdn, sets, &before);
	hpa_shard_sets_mtx_stats_reset(tsdn, sets);
	mutex_prof_data_t r5[3];
	stats_mtx_read(tsdn, sets, r5);
	for (unsigned k = 0; k < 3; k++) {
		expect_u64_eq(r5[k].n_lock_ops, per_read[k],
		    "%s: after a reset a read should count only its own locks",
		    names[k]);
		expect_u64_eq(r5[k].n_wait_times, 0, "%s not reset", names[k]);
		expect_u64_eq(
		    r5[k].n_spin_acquired, 0, "%s not reset", names[k]);
		expect_u64_eq(nstime_ns(&r5[k].tot_wait_time), 0,
		    "%s not reset", names[k]);
		expect_u64_eq(nstime_ns(&r5[k].max_wait_time), 0,
		    "%s not reset", names[k]);
		expect_u32_eq(r5[k].max_n_thds, 0, "%s not reset", names[k]);
	}
	hpa_shard_sets_stats_merge(tsdn, sets, &after);
	expect_d_eq(memcmp(&before, &after, sizeof(before)), 0,
	    "a mutex reset should not change the shards' statistics");

	stats_env_fini(tsdn, &env);
}
TEST_END

TEST_BEGIN(test_stats_walks_without_shards) {
	test_skip_if(!hpa_supported());

	/*
	 * Booted sets whose slots are all still empty, and a zeroed container
	 * like the process-wide sets before boot: the walks find no shard and
	 * leave their outputs untouched.
	 */
	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	real_sets_t     *rs = real_sets_create(tsdn);
	hpa_shard_sets_t unbooted;
	memset(&unbooted, 0, sizeof(unbooted));
	hpa_shard_sets_t *cases[] = {&rs->sets, &unbooted};

	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_shard_stats_t stats, stats_before;
		memset(&stats, 0x5a, sizeof(stats));
		stats_before = stats;
		hpa_shard_sets_stats_merge(tsdn, cases[c], &stats);
		expect_d_eq(memcmp(&stats, &stats_before, sizeof(stats)), 0,
		    "case %u: the merge should leave its output alone", c);

		mutex_prof_data_t data[3], data_before[3];
		memset(data, 0x5a, sizeof(data));
		memcpy(data_before, data, sizeof(data));
		hpa_shard_sets_mtx_stats_read(
		    tsdn, cases[c], &data[0], &data[1], &data[2]);
		expect_d_eq(memcmp(data, data_before, sizeof(data)), 0,
		    "case %u: the read should leave its outputs alone", c);

		expect_zu_eq(hpa_shard_sets_ndirty(cases[c]), 0,
		    "case %u: no shard, no dirty pages", c);
		hpa_shard_sets_mtx_stats_reset(tsdn, cases[c]);
	}
	real_sets_destroy(tsdn, rs);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_route_coverage,
	    test_lazy_init_uses_slot_as_owner_id, test_fork_hooks,
	    test_pick_arena_uses_affinity, test_pick_roundrobin_spreads,
	    test_config_validate, test_config_add, test_pick_from_name,
	    test_stats_merge_walk, test_mtx_stats_walk,
	    test_stats_walks_without_shards);
}
