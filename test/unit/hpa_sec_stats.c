#include "test/jemalloc_test.h"
#include "test/arena_util.h"

#include "jemalloc/internal/hpa.h"

typedef struct {
	size_t active, dirty, resident, global_resident;
	size_t hpa_sec, pac_sec;
} stats_snapshot_t;

static arena_t *
get_arena(unsigned ind) {
	arena_t *arena = arena_get(tsd_tsdn(tsd_fetch()), ind, false);
	assert_ptr_not_null(arena, "Arena must exist");
	return arena;
}

static unsigned
create_arena(bool pac_sec) {
	sec_opts_t saved_hpa = opt_hpa_sec_opts, saved_pac = opt_pac_sec_opts;
	size_t     extent_size = SC_LARGE_MINCLASS + sz_large_pad;
	opt_hpa_sec_opts = (sec_opts_t){1, extent_size, 8 * extent_size, false};
	opt_pac_sec_opts = (sec_opts_t){
	    pac_sec ? 1 : 0, extent_size, 4 * extent_size, false};
	unsigned ind = do_arena_create(-1, -1);
	opt_hpa_sec_opts = saved_hpa;
	opt_pac_sec_opts = saved_pac;
	hpa_shard_t *hpa = &get_arena(ind)->pa_shard.hpa;
	hpa->opts.slab_max_alloc = extent_size;
	hpa->opts.dirty_mult = FXP_INIT_INT(0);
	hpa->opts.hugify_style = hpa_hugify_style_none;
	hpa->opts.hugify_sync = false;
	hpa->opts.min_purge_interval_ms = 0;
	/* Keep purge eligibility current; run maintenance only explicitly. */
	hpa_shard_set_deferral_allowed(tsd_tsdn(tsd_fetch()), hpa, true);
	return ind;
}

static size_t
read_stat(unsigned ind, const char *field) {
	char key[128];
	snprintf(key, sizeof(key), "stats.arenas.%u.%s", ind, field);
	size_t value = 0, len = sizeof(value);
	expect_d_eq(mallctl(key, &value, &len, NULL, 0), 0,
	    "Unexpected mallctl failure reading %s", key);
	return value;
}

static stats_snapshot_t
snapshot(unsigned ind) {
	do_epoch();
	pa_shard_t *shard = &get_arena(ind)->pa_shard;
	sec_stats_t pac = {0}, hpa = {0};
	sec_stats_merge(TSDN_NULL, &shard->pac.sec, &pac);
	sec_stats_merge(TSDN_NULL, &shard->hpa.sec, &hpa);
	expect_zu_eq(pac.bytes_pinned + hpa.bytes_pinned, 0,
	    "Test extents must be unpinned");
	stats_snapshot_t result = {0};
	result.active = read_stat(ind, "pactive") << LG_PAGE;
	result.dirty = read_stat(ind, "pdirty") << LG_PAGE;
	result.hpa_sec = hpa.bytes;
	result.pac_sec = pac.bytes;
	expect_zu_eq(result.dirty,
	    ((ecache_npages_get(&shard->pac.ecache_dirty)
	         + psset_ndirty(&shard->hpa.psset))
	        << LG_PAGE)
	        + hpa.bytes + pac.bytes,
	    "Dirty pages must include cached SEC extents");
	if (config_stats) {
		result.resident = read_stat(ind, "resident");
		size_t len = sizeof(result.global_resident);
		expect_d_eq(mallctl("stats.resident", &result.global_resident,
		                &len, NULL, 0),
		    0, "Unexpected resident mallctl failure");
	}
	return result;
}

static void
expect_same_resident(stats_snapshot_t before, stats_snapshot_t after) {
	if (config_stats) {
		expect_zu_eq(after.resident, before.resident,
		    "Cache transfers without purge must preserve resident");
	}
}

static void
flush_and_destroy(unsigned ind) {
	tsdn_t          *tsdn = tsd_tsdn(tsd_fetch());
	pa_shard_t      *shard = &get_arena(ind)->pa_shard;
	stats_snapshot_t cached = snapshot(ind);
	pa_shard_flush(tsdn, shard, false);
	stats_snapshot_t flushed = snapshot(ind);
	expect_zu_eq(
	    flushed.hpa_sec + flushed.pac_sec, 0, "SECs must be empty");
	expect_zu_eq(
	    flushed.dirty, cached.dirty, "Flush must preserve dirty pages");
	expect_same_resident(cached, flushed);
	if (config_stats) {
		expect_zu_eq(flushed.global_resident, cached.global_resident,
		    "Flush must preserve global resident");
	}
	do_purge(ind);
	hpa_shard_do_deferred_work(tsdn, &shard->hpa);
	stats_snapshot_t purged = snapshot(ind);
	expect_zu_eq(purged.dirty, 0, "Maintenance must purge dirty pages");
	if (config_stats) {
		expect_zu_eq(purged.resident + flushed.dirty, flushed.resident,
		    "Actual purge must remove resident pages");
	}
	do_arena_destroy(ind);
}

TEST_BEGIN(test_hpa_sec_stats) {
	test_skip_if(!opt_hpa || !hpa_supported() || opt_san_guard_large != 0);
	test_skip_if(background_thread_enabled());
	unsigned ind = create_arena(false);
	int      flags = MALLOCX_ARENA(ind) | MALLOCX_TCACHE_NONE;
	void    *ptr = mallocx(SC_LARGE_MINCLASS, flags);
	assert_ptr_not_null(ptr, "HPA allocation failed");
	stats_snapshot_t allocated = snapshot(ind);
	expect_zu_gt(allocated.hpa_sec, 0, "Refill must leave cached extents");
	dallocx(ptr, flags);
	stats_snapshot_t freed = snapshot(ind);
	expect_zu_eq(freed.active, 0, "Freed extent must leave PA active");
	expect_zu_eq(freed.hpa_sec, allocated.hpa_sec + allocated.active,
	    "SEC must absorb the freed extent");
	expect_same_resident(allocated, freed);
	ptr = mallocx(SC_LARGE_MINCLASS, flags);
	assert_ptr_not_null(ptr, "SEC reallocation failed");
	stats_snapshot_t reused = snapshot(ind);
	expect_zu_eq(
	    reused.hpa_sec, allocated.hpa_sec, "Reallocation must reuse SEC");
	expect_same_resident(freed, reused);
	dallocx(ptr, flags);
	flush_and_destroy(ind);
}
TEST_END

TEST_BEGIN(test_mixed_sec_stats) {
	test_skip_if(!opt_hpa || !hpa_supported() || opt_san_guard_large != 0);
	test_skip_if(background_thread_enabled() || (config_prof && opt_prof));
	unsigned    ind = create_arena(true);
	int         flags = MALLOCX_ARENA(ind) | MALLOCX_TCACHE_NONE;
	pa_shard_t *shard = &get_arena(ind)->pa_shard;
	void       *hpa_ptr = mallocx(SC_LARGE_MINCLASS, flags);
	assert_ptr_not_null(hpa_ptr, "HPA allocation failed");
	stats_snapshot_t initial = snapshot(ind);
	pa_shard_disable_hpa(tsd_tsdn(tsd_fetch()), shard);
	expect_same_resident(initial, snapshot(ind));
	void *pac_ptr = mallocx(SC_LARGE_MINCLASS, flags);
	assert_ptr_not_null(pac_ptr, "PAC allocation failed");
	stats_snapshot_t allocated = snapshot(ind);
	dallocx(pac_ptr, flags);
	stats_snapshot_t pac_cached = snapshot(ind);
	expect_zu_gt(pac_cached.pac_sec, 0, "PAC SEC must retain its extent");
	expect_same_resident(allocated, pac_cached);
	dallocx(hpa_ptr, flags);
	stats_snapshot_t cached = snapshot(ind);
	expect_zu_gt(
	    cached.hpa_sec, 0, "HPA SEC must retain its extent after disable");
	expect_zu_gt(cached.pac_sec, 0, "Both SECs must remain populated");
	expect_zu_eq(cached.active, 0, "Both extents must leave PA active");
	expect_same_resident(pac_cached, cached);
	if (config_stats) {
		expect_zu_eq(cached.global_resident, allocated.global_resident,
		    "Cache transfers must preserve global resident");
	}
	flush_and_destroy(ind);
}
TEST_END

int
main(void) {
	/* Initialize the shared HPA backend only on supported platforms. */
	if (hpa_supported()) {
		opt_hpa = true;
	}
	return test_no_reentrancy(test_hpa_sec_stats, test_mixed_sec_stats);
}
