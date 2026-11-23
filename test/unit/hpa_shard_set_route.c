#include "test/jemalloc_test.h"
#include "test/sleep.h"

#include "jemalloc/internal/hpa_shard_set.h"

#ifndef _WIN32
#	include <sys/wait.h>
#endif

/*
 * With the options in the .sh, malloc_init boots two global HPA shard sets,
 * of 2 and 8 shards, and the automatic arenas allocate from them: the usable
 * size picks the set, and the arena index the shard in it.  Manual arenas keep
 * an HPA shard of their own, and every extent is freed to the shard that owns
 * its pageslab.
 */

#define NSETS 2

static bool
sets_booted(void) {
	return hpa_shard_sets_global.initialized && !san_guard_enabled();
}

/* The shard owning the pageslab of ptr's extent, NULL if not from the HPA. */
static hpa_shard_t *
owner_of(const void *ptr) {
	edata_t *edata = emap_edata_lookup(
	    tsd_tsdn(tsd_fetch()), &arena_emap_global, ptr);
	expect_ptr_not_null(edata, "no extent for %p", ptr);
	if (edata == NULL || edata_pai_get(edata) != EXTENT_PAI_HPA) {
		return NULL;
	}
	return hpdata_shard_get(edata_ps_get(edata));
}

/* The shard in the slot an arena picks in the set serving usize, if any. */
static hpa_shard_t *
routed_shard(size_t usize, unsigned arena_ind) {
	hpa_shard_set_t *shard_set = hpa_shard_sets_lookup(
	    &hpa_shard_sets_global, usize);
	if (shard_set == NULL) {
		return NULL;
	}
	return (hpa_shard_t *)atomic_load_p(
	    &shard_set->shards[arena_ind % shard_set->nshards], ATOMIC_ACQUIRE);
}

static size_t
last_set_size_max(void) {
	return hpa_shard_sets_global.sets[hpa_shard_sets_global.nsets - 1]
	    .size_max;
}

TEST_BEGIN(test_boot) {
	test_skip_if(!hpa_supported());

	expect_true(opt_hpa && opt_hpa_shard_sets, "set by main() and the .sh");
	expect_true(hpa_shard_sets_global.initialized,
	    "malloc_init should boot the sets");
	expect_u_eq(hpa_shard_sets_global.nsets, NSETS, "unexpected layout");
	expect_u_eq(hpa_shard_sets_global.sets[0].nshards, 2, "set 0 shards");
	expect_u_eq(hpa_shard_sets_global.sets[1].nshards, 8, "set 1 shards");

	arena_t *a0 = arena_get(tsd_tsdn(tsd_fetch()), 0, false);
	expect_true(pa_shard_on_hpa_sets(&a0->pa_shard),
	    "arena 0 should allocate from the sets");
	expect_true(atomic_load_b(&a0->pa_shard.use_hpa, ATOMIC_RELAXED),
	    "arena 0 should use the HPA");
	expect_false(a0->pa_shard.ever_used_hpa,
	    "arena 0 should have no HPA shard of its own");
}
TEST_END

TEST_BEGIN(test_automatic_arenas) {
	test_skip_if(!sets_booted());

	/* Slab and large sizes, up to the largest the sets serve. */
	const size_t sizes[] = {
	    8, 1024, SC_SMALL_MAXCLASS, SC_LARGE_MINCLASS, last_set_size_max()};
	unsigned narenas = narenas_auto < 6 ? narenas_auto : 6;
	for (unsigned i = 0; i < narenas; i++) {
		for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			size_t usize = sz_s2u(sizes[s]);
			if (usize > last_set_size_max()) {
				continue;
			}
			void *p = mallocx(
			    sizes[s], MALLOCX_ARENA(i) | MALLOCX_TCACHE_NONE);
			expect_ptr_not_null(p, "mallocx(%zu) failed", sizes[s]);
			hpa_shard_t *owner = owner_of(p);
			expect_ptr_not_null(owner,
			    "arena %u, size %zu: should come from the HPA", i,
			    sizes[s]);
			expect_ptr_eq(owner, routed_shard(usize, i),
			    "arena %u, size %zu: from the wrong shard", i,
			    sizes[s]);
			arena_t *arena = arena_get(
			    tsd_tsdn(tsd_fetch()), i, false);
			expect_ptr_ne(owner, &arena->pa_shard.hpa,
			    "arena %u should not use a shard of its own", i);
			dallocx(p, MALLOCX_TCACHE_NONE);
		}
	}
}
TEST_END

TEST_BEGIN(test_manual_arena) {
	test_skip_if(!sets_booted());

	unsigned ind;
	size_t   sz = sizeof(ind);
	expect_d_eq(mallctl("arenas.create", &ind, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	arena_t *arena = arena_get(tsd_tsdn(tsd_fetch()), ind, false);
	expect_false(pa_shard_on_hpa_sets(&arena->pa_shard),
	    "a manual arena should not use the sets");
	expect_true(arena->pa_shard.ever_used_hpa,
	    "a manual arena should have an HPA shard of its own");

	void *p = mallocx(64, MALLOCX_ARENA(ind) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_ptr_eq(owner_of(p), &arena->pa_shard.hpa,
	    "a manual arena should allocate from its own shard");
	dallocx(p, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_beyond_the_sets) {
	test_skip_if(!sets_booted());

	/* The next usable size up, which no set serves, goes to the PAC. */
	size_t size = sz_s2u(last_set_size_max() + 1);
	void  *p = mallocx(size, MALLOCX_ARENA(0) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_ptr_null(
	    owner_of(p), "size %zu should not come from the HPA", size);
	dallocx(p, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_shared_shards) {
	test_skip_if(!sets_booted());
	unsigned n = hpa_shard_sets_global.sets[0].nshards;
	test_skip_if(n < 2 || narenas_auto <= n);

	/*
	 * Arenas 0 and n pick slot 0 of set 0, of n shards, and arena 1 slot
	 * 1: an extent freed by one arena is in the shard the other allocates
	 * from, however it was cached there.
	 */
	size_t size = 64;
	assert_ptr_eq(hpa_shard_sets_lookup(&hpa_shard_sets_global, size),
	    &hpa_shard_sets_global.sets[0], "64 bytes should be in set 0");
	unsigned arenas[3] = {0, 1, n};
	void    *p[3];
	for (unsigned i = 0; i < 3; i++) {
		p[i] = mallocx(
		    size, MALLOCX_ARENA(arenas[i]) | MALLOCX_TCACHE_NONE);
		expect_ptr_not_null(p[i], "Unexpected mallocx() failure");
	}
	expect_ptr_eq(owner_of(p[0]), owner_of(p[2]),
	    "arenas 0 and %u should share a shard", n);
	expect_ptr_ne(owner_of(p[0]), owner_of(p[1]),
	    "arenas 0 and 1 should not share a shard");
	dallocx(p[0], MALLOCX_TCACHE_NONE);
	void *q = mallocx(size, MALLOCX_ARENA(n) | MALLOCX_TCACHE_NONE);
	expect_ptr_eq(owner_of(q), owner_of(p[2]),
	    "arena %u should still allocate from its shard", n);
	dallocx(q, MALLOCX_TCACHE_NONE);
	dallocx(p[1], MALLOCX_TCACHE_NONE);
	dallocx(p[2], MALLOCX_TCACHE_NONE);
}
TEST_END

static size_t
sec_bytes(hpa_shard_t *shard) {
	hpa_shard_stats_t stats;
	memset(&stats, 0, sizeof(stats));
	hpa_shard_stats_merge(tsd_tsdn(tsd_fetch()), shard, &stats);
	return stats.secstats.bytes;
}

static void
purge(const char *arena) {
	char name[64];
	malloc_snprintf(name, sizeof(name), "arena.%s.purge", arena);
	expect_d_eq(mallctl(name, NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
}

TEST_BEGIN(test_purge) {
	test_skip_if(!sets_booted());
	/* With 64 KiB pages, the sets do not serve the size below. */
	test_skip_if(SC_LARGE_MINCLASS > last_set_size_max());

	/*
	 * An arena's purge leaves alone the shards it shares with others, and
	 * purging all arenas flushes them.  The extent is small enough for the
	 * SEC to keep.
	 */
	void *p = mallocx(
	    SC_LARGE_MINCLASS, MALLOCX_ARENA(0) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	hpa_shard_t *shard = owner_of(p);
	assert_ptr_not_null(shard, "should come from the HPA");
	dallocx(p, MALLOCX_TCACHE_NONE);
	size_t cached = sec_bytes(shard);
	test_skip_if(cached == 0);

	purge("0");
	expect_zu_eq(sec_bytes(shard), cached,
	    "arena.0.purge should not flush a shared shard");
	purge(STRINGIFY(MALLCTL_ARENAS_ALL));
	expect_zu_eq(sec_bytes(shard), 0,
	    "purging all arenas should flush the sets' shards");
}
TEST_END

#ifndef _WIN32
static void
wait_for_child_exit(pid_t pid) {
	int status;
	if (waitpid(pid, &status, 0) == -1) {
		test_fail("Unexpected waitpid() failure");
		return;
	}
	if (WIFSIGNALED(status)) {
		test_fail("Child terminated by signal %d", WTERMSIG(status));
	} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		test_fail(
		    "Unexpected child exit value %d", WEXITSTATUS(status));
	}
}

/*
 * Finds a slot of the last set with no shard that an automatic arena picks:
 * arena *ind, below nshards, picks slot *ind.  Returns false if there is none.
 */
static bool
last_set_empty_slot(unsigned *ind) {
	hpa_shard_set_t *last =
	    &hpa_shard_sets_global.sets[hpa_shard_sets_global.nsets - 1];
	for (unsigned i = 0; i < last->nshards && i < narenas_auto; i++) {
		if (atomic_load_p(&last->shards[i], ATOMIC_ACQUIRE) == NULL) {
			*ind = i;
			return true;
		}
	}
	return false;
}

/*
 * In a child, creates the shard in slot ind of the last set by allocating
 * from arena ind, and returns it; _exit()s with code if that fails.
 */
static hpa_shard_t *
child_create_shard(unsigned ind, int code) {
	hpa_shard_set_t *last =
	    &hpa_shard_sets_global.sets[hpa_shard_sets_global.nsets - 1];
	void *p = mallocx(
	    last->size_max, MALLOCX_ARENA(ind) | MALLOCX_TCACHE_NONE);
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &last->shards[ind], ATOMIC_ACQUIRE);
	if (p == NULL || shard == NULL) {
		_exit(code);
	}
	dallocx(p, MALLOCX_TCACHE_NONE);
	return shard;
}

/*
 * In a child, purges every arena, which flushes every shard; _exit()s with
 * code if that fails.
 */
static void
child_purge_all(int code) {
	if (mallctl("arena." STRINGIFY(MALLCTL_ARENAS_ALL) ".purge", NULL, NULL,
	        NULL, 0)
	    != 0) {
		_exit(code);
	}
}
#endif

TEST_BEGIN(test_fork) {
	test_skip_if(!sets_booted());
	test_skip_if(narenas_auto < 2);
#ifndef _WIN32
	/*
	 * Live shards in both sets across the fork; the child frees into them
	 * and flushes every shard.
	 */
	size_t sizes[] = {64, last_set_size_max()};
	void  *ptrs[2][2];
	for (unsigned i = 0; i < 2; i++) {
		for (unsigned s = 0; s < 2; s++) {
			ptrs[i][s] = mallocx(
			    sizes[s], MALLOCX_ARENA(i) | MALLOCX_TCACHE_NONE);
			expect_ptr_not_null(
			    ptrs[i][s], "Unexpected mallocx() failure");
		}
	}

	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		for (unsigned i = 0; i < 2; i++) {
			for (unsigned s = 0; s < 2; s++) {
				dallocx(ptrs[i][s], MALLOCX_TCACHE_NONE);
			}
		}
		child_purge_all(1);
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
	for (unsigned i = 0; i < 2; i++) {
		for (unsigned s = 0; s < 2; s++) {
			dallocx(ptrs[i][s], MALLOCX_TCACHE_NONE);
		}
	}
#endif
}
TEST_END

static void
set_background_thread(bool enable) {
	expect_d_eq(
	    mallctl("background_thread", NULL, NULL, &enable, sizeof(enable)),
	    0, "Unexpected mallctl() failure");
}

/* Every set's, and every shard's, deferral_allowed is allowed. */
static void
expect_deferral_allowed(bool allowed) {
	for (unsigned i = 0; i < hpa_shard_sets_global.nsets; i++) {
		hpa_shard_set_t *shard_set = &hpa_shard_sets_global.sets[i];
		expect_b_eq(
		    shard_set->opts.deferral_allowed, allowed, "set %u", i);
		for (unsigned j = 0; j < shard_set->nshards; j++) {
			hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
			    &shard_set->shards[j], ATOMIC_ACQUIRE);
			if (shard != NULL) {
				expect_b_eq(shard->opts.deferral_allowed,
				    allowed, "set %u, shard %u", i, j);
			}
		}
	}
}

static size_t
shard_ndirty(hpa_shard_t *shard) {
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	malloc_mutex_lock(tsdn, &shard->mtx);
	size_t ndirty = psset_ndirty(&shard->psset);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	return ndirty;
}

TEST_BEGIN(test_background_thread) {
	test_skip_if(!sets_booted());
	test_skip_if(!have_background_thread);
	/* Arena 1's background thread must be another one than thread 0. */
	test_skip_if(narenas_auto < 2 || max_background_threads < 2);

	expect_deferral_allowed(false);
	set_background_thread(true);
	expect_deferral_allowed(true);

	/*
	 * Pages freed past the SEC stay dirty, since application threads defer
	 * purging now, until background thread 0 purges them.  They are freed
	 * once it sleeps with no deadline, so that it purges them only if woken
	 * (should it not sleep so within 10 s, a timed wakeup may purge them).
	 */
	background_thread_info_t *info = &background_thread_info[0];
	for (unsigned ms = 0;
	    !background_thread_indefinite_sleep(info) && ms < 10 * 1000;
	    ms += 10) {
		sleep_ns(10 * 1000 * 1000);
	}
	size_t size = last_set_size_max();
	void  *ptrs[8];
	for (unsigned i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); i++) {
		ptrs[i] = mallocx(size, MALLOCX_ARENA(1) | MALLOCX_TCACHE_NONE);
		expect_ptr_not_null(ptrs[i], "Unexpected mallocx() failure");
	}
	hpa_shard_t *shard = owner_of(ptrs[0]);
	assert_ptr_not_null(shard, "should come from the HPA");
	for (unsigned i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); i++) {
		dallocx(ptrs[i], MALLOCX_TCACHE_NONE);
	}
	size_t ndirty = shard_ndirty(shard);
	for (unsigned ms = 0; ndirty != 0 && ms < 10 * 1000; ms += 10) {
		sleep_ns(10 * 1000 * 1000);
		ndirty = shard_ndirty(shard);
	}
	expect_zu_eq(ndirty, 0, "background thread 0 should purge the shard");

	set_background_thread(false);
	expect_deferral_allowed(false);
}
TEST_END

TEST_BEGIN(test_fork_background_thread) {
	test_skip_if(!sets_booted());
	test_skip_if(!have_background_thread);
#ifndef _WIN32
	/*
	 * A child has no background threads, so the shards it creates do not
	 * defer their work to them (test_fork_new_shard creates one), while the
	 * parent's still do.
	 */
	set_background_thread(true);
	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		hpa_shard_sets_t *sets = &hpa_shard_sets_global;
		for (unsigned i = 0; i < sets->nsets; i++) {
			if (sets->sets[i].opts.deferral_allowed) {
				_exit(1);
			}
		}
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
	expect_deferral_allowed(true);
	set_background_thread(false);
#endif
}
TEST_END

TEST_BEGIN(test_fork_new_shard) {
	test_skip_if(!sets_booted());
#ifndef _WIN32
	/*
	 * The child creates a shard in an empty slot of the last set, which does
	 * not defer its work although background threads were on at the fork,
	 * and flushes it with every other shard.  With the .sh's 8 slots in the
	 * last set, the tests before leave slots 6 and 7 empty.
	 */
	unsigned ind;
	test_skip_if(!last_set_empty_slot(&ind));
	if (have_background_thread) {
		set_background_thread(true);
	}
	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		hpa_shard_t *shard = child_create_shard(ind, 1);
		if (shard->opts.deferral_allowed) {
			_exit(2);
		}
		child_purge_all(3);
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
	if (have_background_thread) {
		set_background_thread(false);
	}
#endif
}
TEST_END

int
main(void) {
	/*
	 * hpa:true in the .sh would fail jemalloc's initialization where the
	 * HPA is not supported, so turn the HPA on here with guards.
	 */
	if (hpa_supported()) {
		opt_hpa = true;
	}
	return test_no_reentrancy(test_boot, test_automatic_arenas,
	    test_manual_arena, test_beyond_the_sets, test_shared_shards,
	    test_purge, test_fork, test_background_thread,
	    test_fork_background_thread, test_fork_new_shard);
}
