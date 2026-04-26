#include "test/jemalloc_test.h"
#include "test/arena_util.h"

#include "jemalloc/internal/arenas_management.h"

/*
 * a0* stuff tested in test/unit/a0, fork related interface in
 * test/unit/fork.  arena_choose_hard is exercised indirectly on each
 * thread's first allocation.
 */

TEST_BEGIN(test_narenas_total_get_consistent) {
	unsigned total = narenas_total_get();
	expect_u_ge(total, narenas_auto,
	    "narenas_total (%u) must be >= narenas_auto (%u)", total,
	    narenas_auto);
	expect_u_gt(total, 0, "narenas_total must be positive after init");
}
TEST_END

TEST_BEGIN(test_narenas_total_set_roundtrip) {
	unsigned saved = narenas_total_get();
	narenas_total_set(saved);
	expect_u_eq(narenas_total_get(), saved,
	    "narenas_total_set/get round-trip mismatch");
}
TEST_END

TEST_BEGIN(test_narenas_auto_set_roundtrip) {
	unsigned saved = narenas_auto;
	narenas_auto_set(saved);
	expect_u_eq(
	    narenas_auto, saved, "narenas_auto_set round-trip mismatch");
}
TEST_END

TEST_BEGIN(test_manual_arena_base_set_roundtrip) {
	unsigned saved = manual_arena_base;
	manual_arena_base_set(saved);
	expect_u_eq(manual_arena_base, saved,
	    "manual_arena_base_set round-trip mismatch");
}
TEST_END

TEST_BEGIN(test_arena_set_roundtrip) {
	tsdn_t  *tsdn = tsd_tsdn(tsd_fetch());
	arena_t *a0 = arena_get(tsdn, 0, false);
	expect_ptr_not_null(a0, "arena 0 should exist after init");

	arena_set(0, a0);
	expect_ptr_eq(arena_get(tsdn, 0, false), a0,
	    "arena_set round-trip mismatch for ind=0");
}
TEST_END

TEST_BEGIN(test_arena_init_creates_arena) {
	tsdn_t  *tsdn = tsd_tsdn(tsd_fetch());
	unsigned before = narenas_total_get();
	expect_u_lt(before, MALLOCX_ARENA_LIMIT,
	    "Cannot create arena: at MALLOCX_ARENA_LIMIT");

	arena_t *arena = arena_init(tsdn, before, &arena_config_default);
	expect_ptr_not_null(arena, "arena_init failed for ind=%u", before);

	expect_u_eq(narenas_total_get(), before + 1,
	    "narenas_total did not increment after arena_init");
	expect_ptr_eq(arena_get(tsdn, before, false), arena,
	    "arena_get does not return the freshly-initialized arena");
}
TEST_END

TEST_BEGIN(test_arena_init_idempotent_auto) {
	test_skip_if(narenas_auto < 1);

	tsdn_t  *tsdn = tsd_tsdn(tsd_fetch());
	arena_t *a0 = arena_get(tsdn, 0, false);
	expect_ptr_not_null(a0, "arena 0 should exist after init");

	unsigned before = narenas_total_get();
	arena_t *again = arena_init(tsdn, 0, &arena_config_default);
	expect_ptr_eq(again, a0,
	    "arena_init for an existing auto arena should return same pointer");
	expect_u_eq(narenas_total_get(), before,
	    "narenas_total should not change for idempotent arena_init");
}
TEST_END

/*
 * test_arena_migrate spawns one worker thread, binds it to arena1, then has
 * it migrate to arena2. We verify the nthreads counters on both arenas and
 * that the migrating thread's tsd_arena was re-pointed.
 */
static unsigned   migrate_a1_ind;
static unsigned   migrate_a2_ind;
static atomic_b_t migrate_done;
static atomic_b_t migrate_go_exit;

static void *
migrate_worker(void *unused) {
	unsigned old_ind;
	size_t   sz = sizeof(unsigned);
	expect_d_eq(mallctl("thread.arena", (void *)&old_ind, &sz,
	                (void *)&migrate_a1_ind, sizeof(unsigned)),
	    0, "thread.arena bind failed");

	tsd_t   *tsd = tsd_fetch();
	tsdn_t  *tsdn = tsd_tsdn(tsd);
	arena_t *a1 = arena_get(tsdn, migrate_a1_ind, false);
	arena_t *a2 = arena_get(tsdn, migrate_a2_ind, false);
	expect_ptr_not_null(a1, "arena1 should exist");
	expect_ptr_not_null(a2, "arena2 should exist");

	arena_migrate(tsd, a1, a2);

	expect_ptr_eq(
	    tsd_arena_get(tsd), a2, "tsd_arena was not updated to newarena");

	atomic_store_b(&migrate_done, true, ATOMIC_RELEASE);

	while (!atomic_load_b(&migrate_go_exit, ATOMIC_ACQUIRE)) {
		/* Hold the binding so main can read post-migration counts. */
	}
	return NULL;
}

TEST_BEGIN(test_arena_migrate) {
	atomic_store_b(&migrate_done, false, ATOMIC_RELEASE);
	atomic_store_b(&migrate_go_exit, false, ATOMIC_RELEASE);

	migrate_a1_ind = do_arena_create(-1, -1);
	migrate_a2_ind = do_arena_create(-1, -1);

	thd_t thd;
	thd_create(&thd, migrate_worker, NULL);

	while (!atomic_load_b(&migrate_done, ATOMIC_ACQUIRE)) {
		/* Wait for the migrator to publish results. */
	}

	tsdn_t  *tsdn = tsdn_fetch();
	arena_t *a1 = arena_get(tsdn, migrate_a1_ind, false);
	arena_t *a2 = arena_get(tsdn, migrate_a2_ind, false);

	expect_u_eq(arena_nthreads_get(a1, false), 0,
	    "arena1 should have 0 threads after migration");
	expect_u_eq(arena_nthreads_get(a2, false), 1,
	    "arena2 should have 1 thread after migration");

	atomic_store_b(&migrate_go_exit, true, ATOMIC_RELEASE);
	thd_join(thd, NULL);

	do_arena_destroy(migrate_a1_ind);
	do_arena_destroy(migrate_a2_ind);
}
TEST_END

static atomic_b_t cleanup_skip_worker;

static void *
arena_cleanup_worker(void *unused) {
	tsd_t *tsd = tsd_fetch();
	/* Bind tsd to both an external and internal arena. */
	free(malloc(1));

	arena_t *a = tsd_arena_get(tsd);
	if (a == NULL) {
		atomic_store_b(&cleanup_skip_worker, true, ATOMIC_RELEASE);
		return NULL;
	}

	unsigned pre_nt = arena_nthreads_get(a, false);

	arena_cleanup(tsd);
	expect_ptr_null(
	    tsd_arena_get(tsd), "tsd_arena should be NULL after arena_cleanup");
	expect_u_eq(arena_nthreads_get(a, false), pre_nt - 1,
	    "external nthreads should decrease by 1 after arena_cleanup");

	arena_cleanup(tsd);
	expect_ptr_null(tsd_arena_get(tsd),
	    "tsd_arena should remain NULL after second arena_cleanup");
	expect_u_eq(arena_nthreads_get(a, false), pre_nt - 1,
	    "external nthreads should not change on idempotent arena_cleanup");

	return NULL;
}

TEST_BEGIN(test_arena_cleanup) {
	atomic_store_b(&cleanup_skip_worker, false, ATOMIC_RELEASE);

	thd_t thd;
	thd_create(&thd, arena_cleanup_worker, NULL);
	thd_join(thd, NULL);

	if (atomic_load_b(&cleanup_skip_worker, ATOMIC_ACQUIRE)) {
		test_skip(
		    "Worker tsd_arena was NULL after malloc; "
		    "cannot exercise arena_cleanup path");
	}
}
TEST_END

static void *
iarena_cleanup_worker(void *unused) {
	tsd_t *tsd = tsd_fetch();
	/* Bind tsd to both an external and internal arena. */
	free(malloc(1));

	arena_t *ia = tsd_iarena_get(tsd);
	if (ia == NULL) {
		atomic_store_b(&cleanup_skip_worker, true, ATOMIC_RELEASE);
		return NULL;
	}

	unsigned pre_nt = arena_nthreads_get(ia, true);

	iarena_cleanup(tsd);
	expect_ptr_null(tsd_iarena_get(tsd),
	    "tsd_iarena should be NULL after iarena_cleanup");
	expect_u_eq(arena_nthreads_get(ia, true), pre_nt - 1,
	    "internal nthreads should decrease by 1 after iarena_cleanup");

	iarena_cleanup(tsd);
	expect_ptr_null(tsd_iarena_get(tsd),
	    "tsd_iarena should remain NULL after second iarena_cleanup");
	expect_u_eq(arena_nthreads_get(ia, true), pre_nt - 1,
	    "internal nthreads should not change on idempotent iarena_cleanup");

	return NULL;
}

TEST_BEGIN(test_iarena_cleanup) {
	atomic_store_b(&cleanup_skip_worker, false, ATOMIC_RELEASE);

	thd_t thd;
	thd_create(&thd, iarena_cleanup_worker, NULL);
	thd_join(thd, NULL);

	if (atomic_load_b(&cleanup_skip_worker, ATOMIC_ACQUIRE)) {
		test_skip(
		    "Worker tsd_iarena was NULL after malloc; "
		    "cannot exercise iarena_cleanup path");
	}
}
TEST_END

int
main(void) {
	return test(test_narenas_total_get_consistent,
	    test_narenas_total_set_roundtrip, test_narenas_auto_set_roundtrip,
	    test_manual_arena_base_set_roundtrip, test_arena_set_roundtrip,
	    test_arena_init_creates_arena, test_arena_init_idempotent_auto,
	    test_arena_migrate, test_arena_cleanup, test_iarena_cleanup);
}
