#include "test/jemalloc_test.h"

#include "jemalloc/internal/pa.h"

#define PAC_TEST_ARENA_IND 124

static bool pac_test_dalloc_fail;
static bool pac_test_purge_lazy_fail;
static unsigned pac_test_alloc_calls;
static unsigned pac_test_dalloc_calls;
static unsigned pac_test_destroy_calls;
static unsigned pac_test_purge_lazy_calls;

static void
pac_test_hooks_reset(void) {
	pac_test_dalloc_fail = false;
	pac_test_purge_lazy_fail = false;
	pac_test_alloc_calls = 0;
	pac_test_dalloc_calls = 0;
	pac_test_destroy_calls = 0;
	pac_test_purge_lazy_calls = 0;
}

static void *
pac_test_alloc(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	pac_test_alloc_calls++;
	void *ret = pages_map(new_addr, size, alignment, commit);
	return ret;
}

static bool
pac_test_dalloc(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	pac_test_dalloc_calls++;
	if (pac_test_dalloc_fail) {
		return true;
	}
	pages_unmap(addr, size);
	return false;
}

static void
pac_test_destroy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	pac_test_destroy_calls++;
	pages_unmap(addr, size);
}

static bool
pac_test_purge_lazy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	pac_test_purge_lazy_calls++;
	return pac_test_purge_lazy_fail;
}

static bool
pac_test_split(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	return !maps_coalesce && !opt_retain;
}

static bool
pac_test_merge(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	return !maps_coalesce && !opt_retain;
}

static extent_hooks_t pac_test_hooks = {
	pac_test_alloc,
	pac_test_dalloc,
	pac_test_destroy,
	NULL, /* commit */
	NULL, /* decommit */
	pac_test_purge_lazy,
	NULL, /* purge_forced */
	pac_test_split,
	pac_test_merge
};

typedef struct pac_test_data_s pac_test_data_t;
struct pac_test_data_s {
	pa_shard_t shard;
	pa_central_t central;
	base_t *base;
	emap_t emap;
	pa_shard_stats_t stats;
	malloc_mutex_t stats_mtx;
	extent_hooks_t hooks;
};

static pac_test_data_t *
pac_test_data_init(bool custom_hooks, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms) {
	tsdn_t *tsdn = tsdn_fetch();
	pac_test_data_t *data = calloc(1, sizeof(*data));
	assert_ptr_not_null(data, "Unexpected calloc failure");

	if (custom_hooks) {
		memcpy(&data->hooks, &pac_test_hooks, sizeof(extent_hooks_t));
	} else {
		memcpy(&data->hooks, &ehooks_default_extent_hooks,
		    sizeof(extent_hooks_t));
	}

	data->base = base_new(tsdn, PAC_TEST_ARENA_IND, &data->hooks,
	    /* metadata_use_hooks */ true);
	assert_ptr_not_null(data->base, "Unexpected base_new failure");
	assert_false(emap_init(&data->emap, data->base, /* zeroed */ true),
	    "Unexpected emap_init failure");
	assert_false(malloc_mutex_init(&data->stats_mtx, "pac_test_stats",
	    WITNESS_RANK_ARENA_STATS, malloc_mutex_rank_exclusive),
	    "Unexpected stats mutex initialization failure");

	nstime_t time;
	nstime_init(&time, 0);
	assert_false(pa_central_init(&data->central, data->base, /* hpa */ false,
	    &hpa_hooks_default), "Unexpected pa_central_init failure");
	assert_false(pa_shard_init(tsdn, &data->shard, &data->central,
	    &data->emap, data->base, PAC_TEST_ARENA_IND, &data->stats,
	    &data->stats_mtx, &time, SC_LARGE_MAXCLASS + PAGE,
	    dirty_decay_ms, muzzy_decay_ms), "Unexpected pa_shard_init failure");

	return data;
}

static void
pac_decay_all_locked(pac_t *pac, extent_state_t state, bool fully_decay) {
	decay_t *decay;
	pac_decay_stats_t *decay_stats;
	ecache_t *ecache;
	if (state == extent_state_dirty) {
		decay = &pac->decay_dirty;
		decay_stats = &pac->stats->decay_dirty;
		ecache = &pac->ecache_dirty;
	} else {
		assert_d_eq(extent_state_muzzy, state,
		    "Only dirty and muzzy decay are supported");
		decay = &pac->decay_muzzy;
		decay_stats = &pac->stats->decay_muzzy;
		ecache = &pac->ecache_muzzy;
	}

	tsdn_t *tsdn = tsdn_fetch();
	malloc_mutex_lock(tsdn, &decay->mtx);
	pac_decay_all(tsdn, pac, decay, decay_stats, ecache, fully_decay);
	malloc_mutex_unlock(tsdn, &decay->mtx);
}

static void
pac_test_data_destroy(pac_test_data_t *data) {
	/*
	 * Decay below calls back into the test hooks; reset all hook state
	 * (including the fail flags) so teardown is unaffected by anything the
	 * preceding test toggled.
	 */
	pac_test_hooks_reset();
	pac_decay_all_locked(&data->shard.pac, extent_state_dirty,
	    /* fully_decay */ true);
	pac_decay_all_locked(&data->shard.pac, extent_state_muzzy,
	    /* fully_decay */ true);
	pa_shard_destroy(tsdn_fetch(), &data->shard);
	base_delete(tsdn_fetch(), data->base);
	/*
	 * pac operations populated the tsd rtree_ctx with leaf-node pointers
	 * from the private emap we just destroyed.  Invalidate the cache so
	 * the next test's fresh emap doesn't follow stale entries.
	 */
	rtree_ctx_data_init(tsd_rtree_ctx(tsd_fetch()));
	free(data);
}

static edata_t *
pac_alloc_expect(pac_test_data_t *data, size_t size, bool guarded) {
	bool deferred_work_generated = false;
	edata_t *edata = pac_alloc(tsdn_fetch(), &data->shard.pac, size, PAGE,
	    /* zero */ false, guarded, /* frequent_reuse */ false,
	    &deferred_work_generated);
	expect_ptr_not_null(edata, "Unexpected pac_alloc failure");
	expect_zu_eq(size, edata_size_get(edata), "Unexpected allocation size");
	return edata;
}

TEST_BEGIN(test_pac_dirty_muzzy_alloc_priority) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;
	size_t alloc_size = HUGEPAGE;
	size_t alloc_npages = alloc_size >> LG_PAGE;

	edata_t *muzzy = pac_alloc_expect(
	    data, alloc_size, /* guarded */ false);
	void *muzzy_addr = edata_base_get(muzzy);
	edata_t *dirty = pac_alloc_expect(
	    data, alloc_size, /* guarded */ false);
	void *dirty_addr = edata_base_get(dirty);

	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, muzzy, &deferred_work_generated);
	pac_decay_all_locked(pac, extent_state_dirty, /* fully_decay */ false);
	expect_zu_eq(alloc_npages, ecache_npages_get(&pac->ecache_muzzy),
	    "Expected one muzzy page after dirty decay");

	deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, dirty, &deferred_work_generated);
	expect_zu_eq(alloc_npages, ecache_npages_get(&pac->ecache_dirty),
	    "Expected one dirty page");

	edata_t *from_dirty = pac_alloc_expect(
	    data, alloc_size, /* guarded */ false);
	expect_ptr_eq(dirty_addr, edata_base_get(from_dirty),
	    "Dirty cache should be preferred over muzzy");

	edata_t *from_muzzy = pac_alloc_expect(
	    data, alloc_size, /* guarded */ false);
	expect_ptr_eq(muzzy_addr, edata_base_get(from_muzzy),
	    "Muzzy cache should be used after dirty cache");

	deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, from_dirty, &deferred_work_generated);
	deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, from_muzzy, &deferred_work_generated);

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_batched_grow_caches_trailing_dirty) {
	test_skip_if(!sz_large_size_classes_disabled()
	    || !(maps_coalesce || opt_retain));

	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	size_t size = HUGEPAGE + PAGE;
	size_t batched_size = sz_s2u_compute_using_delta(size);
	size_t next_hugepage_size = HUGEPAGE_CEILING(size);
	if (batched_size > next_hugepage_size) {
		batched_size = next_hugepage_size;
	}
	assert_zu_gt(batched_size, size,
	    "Test size should exercise batched retained growth");

	size_t dirty_before = ecache_npages_get(&pac->ecache_dirty);
	edata_t *large = pac_alloc_expect(data, size, /* guarded */ false);
	expect_zu_eq(dirty_before + ((batched_size - size) >> LG_PAGE),
	    ecache_npages_get(&pac->ecache_dirty),
	    "Batched grow should cache the trailing dirty extent");
	if (config_stats) {
		expect_zu_ge(pac_mapped(pac), batched_size,
		    "Mapped stats should include the full batched grow");
	}

	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, large, &deferred_work_generated);

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_deferred_work_signals) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	edata_t *edata = pac_alloc_expect(data, PAGE, /* guarded */ false);
	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, edata, &deferred_work_generated);
	expect_true(deferred_work_generated,
	    "Non-pinned dalloc should request deferred work");

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_decay_dirty_to_muzzy_via_purge_lazy) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	edata_t *edata = pac_alloc_expect(data, PAGE, /* guarded */ false);
	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, edata, &deferred_work_generated);

	unsigned purge_lazy_before = pac_test_purge_lazy_calls;
	pac_decay_all_locked(pac, extent_state_dirty, /* fully_decay */ false);
	expect_zu_eq(0, ecache_npages_get(&pac->ecache_dirty),
	    "Dirty decay should remove dirty pages");
	expect_zu_eq(1, ecache_npages_get(&pac->ecache_muzzy),
	    "Successful lazy purge should move dirty pages to muzzy");
	expect_u_gt(pac_test_purge_lazy_calls, purge_lazy_before,
	    "Dirty-to-muzzy decay should call purge_lazy");

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_decay_retains_when_dalloc_fails) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	pac_test_dalloc_fail = true;
	edata_t *edata = pac_alloc_expect(data, PAGE, /* guarded */ false);
	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, edata, &deferred_work_generated);
	size_t retained_before = ecache_npages_get(&pac->ecache_retained);
	pac_decay_all_locked(pac, extent_state_dirty, /* fully_decay */ true);
	expect_zu_gt(ecache_npages_get(&pac->ecache_retained), retained_before,
	    "Fully decayed dirty pages should be retained when dalloc fails");
	expect_u_gt(pac_test_dalloc_calls, 0,
	    "Fully decayed dirty pages should attempt dalloc first");

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_non_pinned_dalloc_signals_deferred_work) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	size_t normal_size = 3 * HUGEPAGE + PAGE;
	edata_t *normal = pac_alloc_expect(data, normal_size,
	    /* guarded */ false);
	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), pac, normal, &deferred_work_generated);
	expect_true(deferred_work_generated,
	    "Non-pinned dalloc should request deferred work");

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_non_pinned_shrink_signals_deferred_work) {
	test_skip_if(!maps_coalesce);

	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	pac_t *pac = &data->shard.pac;

	size_t normal_size = 3 * HUGEPAGE + PAGE;
	size_t normal_shrink_size = 3 * HUGEPAGE;
	edata_t *normal = pac_alloc_expect(data, normal_size,
	    /* guarded */ false);
	bool deferred_work_generated = false;
	expect_false(pac_shrink(tsdn_fetch(), pac, normal, normal_size,
	    normal_shrink_size, &deferred_work_generated),
	    "Unexpected non-pinned shrink failure");
	expect_true(deferred_work_generated,
	    "Non-pinned shrink should request deferred work");
	pac_dalloc(tsdn_fetch(), pac, normal, &deferred_work_generated);

	pac_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_pac_large_guarded_dalloc_unguards_before_caching) {
	pac_test_hooks_reset();
	pac_test_data_t *data = pac_test_data_init(
	    /* custom_hooks */ true, -1, -1);
	edata_t *guarded = pac_alloc_expect(data, SC_LARGE_MINCLASS,
	    /* guarded */ true);
	expect_true(edata_guarded_get(guarded),
	    "Guarded allocation should set the guarded bit");
	bool deferred_work_generated = false;
	pac_dalloc(tsdn_fetch(), &data->shard.pac, guarded,
	    &deferred_work_generated);
	expect_true(deferred_work_generated,
	    "Guarded dalloc should still request deferred work");
	expect_false(edata_guarded_get(guarded),
	    "Large guarded dalloc should unguard before caching");
	pac_test_data_destroy(data);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_pac_dirty_muzzy_alloc_priority,
	    test_pac_batched_grow_caches_trailing_dirty,
	    test_pac_deferred_work_signals,
	    test_pac_decay_dirty_to_muzzy_via_purge_lazy,
	    test_pac_decay_retains_when_dalloc_fails,
	    test_pac_non_pinned_dalloc_signals_deferred_work,
	    test_pac_non_pinned_shrink_signals_deferred_work,
	    test_pac_large_guarded_dalloc_unguards_before_caching);
}
