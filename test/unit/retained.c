#include "test/jemalloc_test.h"

#include "jemalloc/internal/san.h"
#include "jemalloc/internal/spin.h"

/*
 * With opt_retain, an arena grows by mapping extents whose sizes follow an
 * exponential series of page size classes (exp_grow).  The series starts at
 * the same class on every platform.
 */

static unsigned arena_ind;
static size_t   sz;
#define NEPOCHS 8
static atomic_u_t epoch;
static atomic_u_t nfinished;

static unsigned
do_arena_create(void) {
	unsigned ind;
	size_t   ind_sz = sizeof(ind);
	expect_d_eq(mallctl("arenas.create", (void *)&ind, &ind_sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	/*
	 * The tests check PAC, so keep HPA off for this arena.  Setting its
	 * extent hooks turns HPA off; set them to the default ones it has.
	 */
	size_t mib[3];
	size_t miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.extent_hooks", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)ind;
	extent_hooks_t *hooks;
	size_t          hooks_sz = sizeof(hooks);
	expect_d_eq(
	    mallctlbymib(mib, miblen, (void *)&hooks, &hooks_sz, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
	expect_d_eq(mallctlbymib(
	                mib, miblen, NULL, NULL, (void *)&hooks, sizeof(hooks)),
	    0, "Unexpected mallctlbymib() failure");

	/*
	 * Disable purging: it moves dirty pages to retained whenever decay
	 * runs, e.g. on a background thread, which would make retained memory
	 * depend on timing.
	 */
	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.dirty_decay_ms", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)ind;
	ssize_t decay_ms = -1;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, (void *)&decay_ms,
	                sizeof(decay_ms)),
	    0, "Unexpected mallctlbymib() failure");
	return ind;
}

static void
do_arena_destroy(unsigned ind) {
	size_t mib[3];
	size_t miblen;

	miblen = sizeof(mib) / sizeof(size_t);
	expect_d_eq(mallctlnametomib("arena.0.destroy", mib, &miblen), 0,
	    "Unexpected mallctlnametomib() failure");
	mib[1] = (size_t)ind;
	expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctlbymib() failure");
}

static void
do_refresh(void) {
	uint64_t refresh_epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, (void *)&refresh_epoch,
	                sizeof(refresh_epoch)),
	    0, "Unexpected mallctl() failure");
}

static size_t
do_get_size_impl(const char *cmd, unsigned ind) {
	size_t mib[4];
	size_t miblen = sizeof(mib) / sizeof(size_t);
	size_t z = sizeof(size_t);

	expect_d_eq(mallctlnametomib(cmd, mib, &miblen), 0,
	    "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	size_t size;
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&size, &z, NULL, 0), 0,
	    "Unexpected mallctlbymib([\"%s\"], ...) failure", cmd);

	return size;
}

static size_t
do_get_active(unsigned ind) {
	return do_get_size_impl("stats.arenas.0.pactive", ind) * PAGE;
}

static size_t
do_get_mapped(unsigned ind) {
	return do_get_size_impl("stats.arenas.0.mapped", ind);
}

static size_t
do_get_retained(unsigned ind) {
	return do_get_size_impl("stats.arenas.0.retained", ind);
}

/*
 * The arena's growth index: its next growth maps this size class, or a later
 * one if the extent does not fit.
 */
static pszind_t
do_get_grow_next(unsigned ind) {
	tsdn_t *tsdn = tsdn_fetch();
	pac_t  *pac = &arena_get(tsdn, ind, false)->pa_shard.pac;
	malloc_mutex_lock(tsdn, &pac->grow_mtx);
	pszind_t next = pac->exp_grow.next;
	malloc_mutex_unlock(tsdn, &pac->grow_mtx);
	return next;
}

/* Returns the allocation's extent size: its usable size plus the large pad. */
static size_t
do_alloc(size_t size) {
	void *p = mallocx(size, MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	return nallocx(size, 0) + sz_large_pad;
}

/* allocated <= active <= mapped must hold at all times. */
static void
expect_usage(unsigned ind, size_t allocated) {
	size_t active = do_get_active(ind);
	expect_zu_le(allocated, active, "Unexpected active memory");
	expect_zu_le(active, do_get_mapped(ind), "Unexpected mapped memory");
}

TEST_BEGIN(test_retained_grow_series) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_retain);

	arena_ind = do_arena_create();
	pszind_t start = do_get_grow_next(arena_ind);
	size_t   allocated = 0;

	/*
	 * The first growth maps the first class of the series.  A request of a
	 * quarter of that class leaves room for another one.
	 */
	size_t small = sz_pind2sz(start) / 4;
	allocated += do_alloc(small);
	pszind_t next = do_get_grow_next(arena_ind);
	expect_u_eq(next, start + 1, "First growth should map class %u", start);

	/* The leftover is retained and serves the next request. */
	allocated += do_alloc(small);
	expect_u_eq(do_get_grow_next(arena_ind), next,
	    "A request that fits in retained memory should not grow");

	/* A request larger than the next class skips to a class that fits. */
	size_t big = 2 * sz_pind2sz(next);
	allocated += do_alloc(big);
	pszind_t skipped_from = next;
	next = do_get_grow_next(arena_ind);
	expect_u_gt(next, skipped_from + 1,
	    "A request larger than class %u should skip it", skipped_from);
	expect_zu_ge(sz_pind2sz(next - 1), big,
	    "The class skipped to should fit the request");

	/*
	 * The class skipped to was the first one that fit, so no leftover fits
	 * the same request again; it grows into the very next class.
	 */
	allocated += do_alloc(big);
	expect_u_eq(do_get_grow_next(arena_ind), next + 1,
	    "Growth should continue after the class skipped to");

	do_refresh();
	expect_usage(arena_ind, allocated);
	do_arena_destroy(arena_ind);
}
TEST_END

static void *
thd_start(void *arg) {
	for (unsigned next_epoch = 1; next_epoch < NEPOCHS; next_epoch++) {
		/* Busy-wait for next epoch. */
		unsigned cur_epoch;
		spin_t   spinner = SPIN_INITIALIZER;
		while ((cur_epoch = atomic_load_u(&epoch, ATOMIC_ACQUIRE))
		    != next_epoch) {
			spin_adaptive(&spinner);
		}
		expect_u_eq(cur_epoch, next_epoch, "Unexpected epoch");

		/* The main thread destroys the arena; no need to free. */
		do_alloc(sz);

		/* Let the main thread know we've finished this iteration. */
		atomic_fetch_add_u(&nfinished, 1, ATOMIC_RELEASE);
	}

	return NULL;
}

TEST_BEGIN(test_retained_grow_concurrent) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_retain);
	/*
	 * Guard decisions are made per thread, so one thread cannot reproduce
	 * what many threads get.
	 */
	test_skip_if(san_guard_enabled());

	arena_ind = do_arena_create();
	pszind_t start = do_get_grow_next(arena_ind);
	sz = sz_pind2sz(start);

	atomic_store_u(&epoch, 0, ATOMIC_RELAXED);

	unsigned nthreads = ncpus * 2;
	if (nthreads > 16) {
		/*
		 * Limit number of threads we are creating for following
		 * reasons.
		 * 1. On 32-bit platforms could run out of vaddr.
		 * 2. On boxes with a lot of CPUs we might have not enough
		 *    memory to fit thd_t into VARIABLE_ARRAY.
		 */
		nthreads = 16;
	}

	/*
	 * Reference run: the same requests, made one at a time on a fresh
	 * arena.  Growth is serialized, so the racing requests below must end
	 * with the same growth index and retained memory.  Requests the size
	 * of the first class make most of them grow, so the threads race on
	 * growth.
	 */
	size_t allocated = 0;
	for (unsigned i = 0; i < nthreads; i++) {
		allocated += do_alloc(sz);
	}
	do_refresh();
	expect_usage(arena_ind, allocated);
	pszind_t ref_next = do_get_grow_next(arena_ind);
	size_t   ref_retained = do_get_retained(arena_ind);
	expect_u_ge(ref_next, start + 2, "Requests should grow more than once");
	do_arena_destroy(arena_ind);
	expect_u_eq(do_arena_create(), arena_ind, "Unexpected arena index");

	VARIABLE_ARRAY(thd_t, threads, nthreads);
	for (unsigned i = 0; i < nthreads; i++) {
		thd_create(&threads[i], thd_start, NULL);
	}

	for (unsigned e = 1; e < NEPOCHS; e++) {
		atomic_store_u(&nfinished, 0, ATOMIC_RELEASE);
		atomic_store_u(&epoch, e, ATOMIC_RELEASE);

		/* Wait for threads to finish allocating. */
		spin_t spinner = SPIN_INITIALIZER;
		while (atomic_load_u(&nfinished, ATOMIC_ACQUIRE) < nthreads) {
			spin_adaptive(&spinner);
		}

		/*
		 * Matching the index alone could miss two threads growing the
		 * same class; the retained memory would differ.
		 */
		do_refresh();
		expect_usage(arena_ind, allocated);
		expect_u_eq(do_get_grow_next(arena_ind), ref_next,
		    "Concurrent requests should reach the same growth index");
		expect_zu_eq(do_get_retained(arena_ind), ref_retained,
		    "Concurrent requests should retain the same memory");

		/*
		 * Clean up arena.  Destroying and recreating the arena
		 * is simpler than specifying extent hooks that deallocate
		 * (rather than retaining) during reset.
		 */
		do_arena_destroy(arena_ind);
		expect_u_eq(
		    do_arena_create(), arena_ind, "Unexpected arena index");
	}

	for (unsigned i = 0; i < nthreads; i++) {
		thd_join(threads[i], NULL);
	}

	do_arena_destroy(arena_ind);
}
TEST_END

int
main(void) {
	return test(test_retained_grow_series, test_retained_grow_concurrent);
}
