#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa_shard_set.h"

/*
 * Except where noted, these tests exercise the routing logic in isolation: no
 * shards are constructed, so no hugepages are mapped.  They check the two
 * decisions the design makes: 1. size to shard set, from the fixed bands and
 * the shard count of each, and 2. shard set to shard.
 */

/* Real objects used only to give the test slots distinct shard pointers. */
#define TEST_MAX_SHARDS 16
static hpa_shard_t test_shards[TEST_MAX_SHARDS];
/* Slot storage for hand-built sets, carved up in shard_sets_init(). */
static atomic_p_t test_slots[TEST_MAX_SHARDS];

static void
shard_sets_init(hpa_shard_sets_t *sets, unsigned nsets, const size_t *bounds,
    const unsigned *nshards) {
	memset(sets, 0, sizeof(*sets));
	sets->nsets = nsets;
	unsigned next_shard = 0;
	for (unsigned i = 0; i < nsets; i++) {
		assert_u_le(next_shard + nshards[i], TEST_MAX_SHARDS,
		    "test configuration too large");
		sets->sets[i].size_max = bounds[i];
		sets->sets[i].nshards = nshards[i];
		sets->sets[i].shards = &test_slots[next_shard];
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

/* Real shard sets with their own base, for tests that create shards. */
typedef struct real_sets_s {
	base_t          *base;
	edata_cache_t    edata_cache;
	emap_t           emap;
	hpa_central_t    central;
	hpa_shard_sets_t sets;
} real_sets_t;

/* Allocates and boots two sets of four slots each; all slots start empty. */
static real_sets_t *
real_sets_create(tsdn_t *tsdn) {
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

	/* Set 0 is the first band, set 1 every larger size the HPA serves. */
	hpa_shard_sets_config_t config;
	memset(&config, 0, sizeof(config));
	config.nshards[0] = 4;
	config.nshards[1] = 4;

	memset(&rs->sets, 0, sizeof(rs->sets));
	assert_false(hpa_shard_sets_boot(tsdn, &rs->sets, rs->base,
	    &rs->central, &rs->emap, &rs->edata_cache, &config, &opt_hpa_opts,
	    &opt_hpa_sec_opts), "booting the shard sets failed");
	return rs;
}

static void
real_sets_destroy(tsdn_t *tsdn, real_sets_t *rs) {
	base_delete(tsdn, rs->base);
	free(rs);
}

/*
 * Whether hpa_alloc() takes the extent behind an allocation of usize: a slab
 * for every small size, and a large extent, usize plus sz_large_pad, up to
 * slab_max_alloc.
 */
static bool
hpa_serves(size_t usize, size_t slab_max_alloc) {
	return usize <= SC_SMALL_MAXCLASS
	    || usize + sz_large_pad <= slab_max_alloc;
}

/*
 * Checks a booted layout against the band rule, worked out independently for
 * every usable size up to HUGEPAGE: usize falls in the first band whose bound
 * covers it and routes to the set started by the nearest band at or below that
 * one with shards, or to no set if the HPA does not serve it.
 */
static void
expect_layout(hpa_shard_sets_t *sets, const hpa_shard_sets_config_t *config,
    size_t slab_max_alloc) {
	unsigned nsets = 0;
	for (size_t usize = sz_s2u(1); usize <= HUGEPAGE;
	    usize = sz_s2u(usize + 1)) {
		hpa_shard_set_t *shard_set = hpa_shard_sets_lookup(sets, usize);
		if (!hpa_serves(usize, slab_max_alloc)) {
			expect_ptr_null(shard_set,
			    "the HPA does not serve size %zu", usize);
			continue;
		}
		unsigned band = 0;
		while (usize > hpa_shard_set_band_max[band]) {
			band++;
		}
		unsigned set = 0;
		unsigned start = 0;
		for (unsigned b = 1; b <= band; b++) {
			if (config->nshards[b] != 0) {
				set++;
				start = b;
			}
		}
		expect_ptr_eq(shard_set, &sets->sets[set],
		    "size %zu in band %u should route to set %u", usize, band,
		    set);
		expect_u_eq(sets->sets[set].nshards, config->nshards[start],
		    "set %u should have the shards of band %u", set, start);
		nsets = set + 1;
	}
	expect_u_eq(sets->nsets, nsets, "unexpected number of sets");
	expect_ptr_null(hpa_shard_sets_lookup(sets, sz_s2u(HUGEPAGE + 1)),
	    "a size above the last band should have no set");
}

TEST_BEGIN(test_boot_layout) {
	test_skip_if(!hpa_supported());

	/* The bands are fixed: 4K to 512K by powers of two, then HUGEPAGE. */
	for (unsigned i = 0; i < HPA_SHARD_SET_NBANDS - 1; i++) {
		expect_zu_eq(hpa_shard_set_band_max[i], (size_t)4096 << i,
		    "band %u", i);
	}
	expect_zu_eq(hpa_shard_set_band_max[HPA_SHARD_SET_NBANDS - 1], HUGEPAGE,
	    "the last band should end at HUGEPAGE");

	/*
	 * Booting builds the layout but creates no shards: one set per band
	 * with shards, extended over the bands without shards above it, ending
	 * at the largest usable size the HPA serves.
	 */
	struct {
		unsigned nshards[HPA_SHARD_SET_NBANDS];
		size_t   slab_max_alloc;
	} cases[] = {
	    /* Every band starts its own set. */
	    {{1, 2, 3, 4, 5, 6, 7, 8, 9}, HUGEPAGE},
	    /* One set for every size. */
	    {{4}, HUGEPAGE},
	    /* Bands without shards join the set of the band below. */
	    {{4, 0, 2, 0, 0, 1, 0, 0, 3}, HUGEPAGE},
	    /* Large sizes get sets while their extents fit slab_max_alloc. */
	    {{4, 0, 2, 0, 0, 1, 0, 0, 3}, 64 * 1024},
	    {{4, 0, 2, 0, 0, 1, 0, 0, 3}, 100 * 1024},
	    /* Only small sizes get sets until a large extent fits. */
	    {{4, 0, 2, 0, 0, 1, 0, 0, 3}, PAGE},
	    {{4, 0, 2, 0, 0, 1, 0, 0, 3}, SC_SMALL_MAXCLASS + sz_large_pad / 2},
	};
	tsdn_t      *tsdn = tsd_tsdn(tsd_fetch());
	real_sets_t *rs = real_sets_create(tsdn);
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_shard_sets_config_t config;
		memcpy(config.nshards, cases[c].nshards,
		    sizeof(config.nshards));
		hpa_shard_opts_t opts = opt_hpa_opts;
		opts.slab_max_alloc = cases[c].slab_max_alloc;

		hpa_shard_sets_t sets;
		memset(&sets, 0, sizeof(sets));
		assert_false(hpa_shard_sets_boot(tsdn, &sets, rs->base,
		    &rs->central, &rs->emap, &rs->edata_cache, &config, &opts,
		    &opt_hpa_sec_opts), "case %u: booting failed", c);
		expect_layout(&sets, &config, opts.slab_max_alloc);
	}
	real_sets_destroy(tsdn, rs);
}
TEST_END

TEST_BEGIN(test_lazy_init) {
	test_skip_if(!hpa_supported());

	/*
	 * Unlike the tests above, this constructs real shards: picking the same
	 * slot in two sets must create a shard for each set, publish it in that
	 * slot, and record it as the owner of the pageslabs it allocates from.
	 */
	tsdn_t     *tsdn = tsd_tsdn(tsd_fetch());
	real_sets_t *rs = real_sets_create(tsdn);
	hpa_shard_sets_t *sets = &rs->sets;
	for (unsigned i = 0; i < 2; i++) {
		for (unsigned j = 0; j < sets->sets[i].nshards; j++) {
			expect_ptr_null(atomic_load_p(&sets->sets[i].shards[j],
			    ATOMIC_ACQUIRE),
			    "set %u: slot %u should start empty", i, j);
		}
	}

	unsigned     slot = 2;
	hpa_shard_t *shards[2];
	for (unsigned i = 0; i < 2; i++) {
		shards[i] = hpa_shard_choose(tsdn, &sets->sets[i], slot);
		assert_ptr_not_null(shards[i], "set %u: lazy init failed", i);
		expect_ptr_eq(atomic_load_p(&sets->sets[i].shards[slot],
		    ATOMIC_ACQUIRE), shards[i],
		    "set %u: the shard should be in its slot", i);

		bool     deferred_work_generated = false;
		edata_t *edata = hpa_alloc(tsdn, shards[i], PAGE, PAGE,
		    /* zero */ false, /* guarded */ false,
		    /* frequent_reuse */ false, &deferred_work_generated);
		assert_ptr_not_null(edata, "set %u: hpa_alloc failed", i);
		expect_ptr_eq(hpdata_shard_get(edata_ps_get(edata)), shards[i],
		    "set %u: the pageslab should record its shard", i);
		hpa_dalloc(tsdn, shards[i], edata, &deferred_work_generated);
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
	live[0] = hpa_shard_choose(tsdn, &sets->sets[0], 0);
	live[1] = hpa_shard_choose(tsdn, &sets->sets[0], 3);
	live[2] = hpa_shard_choose(tsdn, &sets->sets[1], 1);
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
	expect_ptr_eq(hpa_shard_choose(tsdn, &sets->sets[0], 0), live[0],
	    "an existing shard should still be returned");
	hpa_shard_t *fresh = hpa_shard_choose(tsdn, &sets->sets[1], 2);
	expect_ptr_not_null(fresh, "lazy init should work after the hooks");
	expect_ptr_eq(atomic_load_p(&sets->sets[1].shards[2], ATOMIC_ACQUIRE),
	    fresh, "the new shard should be published");

	real_sets_destroy(tsdn, rs);
}
TEST_END

TEST_BEGIN(test_pick_by_affinity) {
	/*
	 * With the arena index as the affinity, one set spanning all sizes with
	 * one shard per arena reproduces the per-arena layout: arena i always
	 * gets shard i.  Arenas past the slot count wrap around.
	 */
	hpa_shard_sets_t sets;
	unsigned         narenas = 10;
	size_t           bounds[] = {HUGEPAGE};
	unsigned         nshards[] = {narenas};
	shard_sets_init(&sets, 1, bounds, nshards);

	for (unsigned arena = 0; arena < narenas; arena++) {
		for (size_t size = 8; size <= HUGEPAGE; size *= 2) {
			expect_u_eq(route_to_test_shard(&sets, size, arena),
			    arena, "arena %u, size %zu: must route to shard %u",
			    arena, size, arena);
		}
		expect_u_eq(route_to_test_shard(&sets, PAGE, narenas + arena),
		    arena, "arena %u should share shard %u", narenas + arena,
		    arena);
	}
}
TEST_END

TEST_BEGIN(test_config_validate) {
	hpa_shard_sets_config_t config;
	memset(&config, 0, sizeof(config));
	config.nshards[0] = 4;
	expect_false(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "one set for every size rejected");

	config.nshards[0] = 0;
	config.nshards[1] = 4;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "a first band without shards accepted");
	config.nshards[0] = 4;

	/* Every band's count is checked, not just the first. */
	config.nshards[HPA_SHARD_SET_NBANDS - 1] = HPA_MAX_SHARDS_PER_SET + 1;
	expect_true(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "a band with more shards than slots accepted");
	config.nshards[HPA_SHARD_SET_NBANDS - 1] = HPA_MAX_SHARDS_PER_SET;
	expect_false(hpa_shard_sets_config_validate(&config, HUGEPAGE),
	    "a band with as many shards as slots rejected");

	/* The bands must cover every usable size the HPA serves. */
	expect_false(hpa_shard_sets_config_validate(
	    &config, HUGEPAGE + sz_large_pad),
	    "large sizes up to the last band rejected");
	expect_true(hpa_shard_sets_config_validate(
	    &config, HUGEPAGE + sz_large_pad + PAGE),
	    "a large size above the last band accepted");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_boot_layout, test_lazy_init,
	    test_fork_hooks, test_pick_by_affinity, test_config_validate);
}
