#include "test/jemalloc_test.h"

/*
 * Under percpu_arena, binding a thread to a manual arena (an index at or above
 * the per-CPU auto range) is one-way: percpu never reclaims it (see
 * arena_choose_impl). Setting thread.arena back to an index in the auto range
 * resumes per-CPU selection instead of failing with EPERM.
 */
TEST_BEGIN(test_thread_arena_resume_percpu) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena));

	unsigned limit = percpu_arena_ind_limit();
	/* Bypass the tcache so every allocation and free hits the arena. */
	const int flags = MALLOCX_TCACHE_NONE;

	void *warm = mallocx(1, 0);
	expect_ptr_not_null(warm, "Unexpected mallocx() failure");
	dallocx(warm, 0);

	unsigned cur;
	size_t sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit, "Thread should start on a per-CPU arena");

	unsigned manual;
	sz = sizeof(manual);
	expect_d_eq(mallctl("arenas.create", (void *)&manual, &sz, NULL, 0), 0,
	    "Unexpected arenas.create() failure");
	expect_u_ge(manual, limit, "A manual arena is outside the per-CPU range");

	unsigned old;
	sz = sizeof(old);
	expect_d_eq(mallctl("thread.arena", (void *)&old, &sz, (void *)&manual,
	    sizeof(manual)), 0, "Binding to a manual arena should be allowed");
	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(cur, manual, "Thread should be bound to the manual arena");

	void *p_manual = mallocx(1024, flags);
	expect_ptr_not_null(p_manual, "Unexpected mallocx() failure");
	unsigned found;
	sz = sizeof(found);
	expect_d_eq(mallctl("arenas.lookup", (void *)&found, &sz,
	    (void *)&p_manual, sizeof(p_manual)), 0,
	    "Unexpected arenas.lookup() failure");
	expect_u_eq(found, manual, "Allocation should come from the manual arena");

	void *scratch = mallocx(1024, flags);
	expect_ptr_not_null(scratch, "Unexpected mallocx() failure");
	dallocx(scratch, flags);

	unsigned resume = 0;
	expect_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&resume,
	    sizeof(resume)), 0, "Should resume per-CPU selection, not fail");
	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit, "Thread should be back on a per-CPU arena");

	void *p_percpu = mallocx(1024, flags);
	expect_ptr_not_null(p_percpu, "Unexpected mallocx() failure");
	sz = sizeof(found);
	expect_d_eq(mallctl("arenas.lookup", (void *)&found, &sz,
	    (void *)&p_percpu, sizeof(p_percpu)), 0,
	    "Unexpected arenas.lookup() failure");
	expect_u_lt(found, limit, "Allocation should come from a per-CPU arena");
	dallocx(p_percpu, flags);

	/* Free the manual-arena region while bound to a different arena. */
	dallocx(p_manual, flags);
}
TEST_END

static void *
percpu_arena_migrate_worker(void *arg) {
	unsigned *cpus = (unsigned *)arg;

	expect_false(os_cpu_set_affinity((int)cpus[0]),
	    "Could not pin to first CPU");
	/* Force the allocation through arena_choose() on a tcache refill. */
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	unsigned first_arena = percpu_arena_choose();
	void *first = mallocx(1024, 0);
	expect_ptr_not_null(first, "Unexpected mallocx() failure");

	unsigned actual;
	size_t sz = sizeof(actual);
	expect_d_eq(mallctl("arenas.lookup", (void *)&actual, &sz,
	    (void *)&first, sizeof(first)), 0, "Unexpected mallctl() failure");
	expect_u_eq(actual, first_arena,
	    "Allocation should come from the first CPU's arena");
	dallocx(first, 0);
	/* Make the post-migration allocation refill the tcache as well. */
	expect_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	expect_false(os_cpu_set_affinity((int)cpus[1]),
	    "Could not pin to second CPU");
	for (unsigned i = 0; i < 1000 && os_cpu_current() != (int)cpus[1];
	    i++) {
		os_cpu_yield();
	}
	expect_d_eq(os_cpu_current(), (int)cpus[1],
	    "Thread did not migrate to the second CPU");

	unsigned second_arena = percpu_arena_choose();
	expect_u_ne(second_arena, first_arena,
	    "Distinct CPUs should select distinct per-CPU arenas");
	void *second = mallocx(1024, 0);
	expect_ptr_not_null(second, "Unexpected mallocx() failure");
	sz = sizeof(actual);
	expect_d_eq(mallctl("arenas.lookup", (void *)&actual, &sz,
	    (void *)&second, sizeof(second)), 0, "Unexpected mallctl() failure");
	expect_u_eq(actual, second_arena,
	    "Allocation should come from the second CPU's arena");
	dallocx(second, 0);
	return NULL;
}

TEST_BEGIN(test_thread_arena_follows_cpu) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena) || !opt_tcache);

	unsigned available[2];
	unsigned ncpus = os_cpu_affinity_cpus(available, 2);
	test_skip_if(ncpus < 2);

	int current_cpu = os_cpu_current();
	test_skip_if(current_cpu < 0);
	unsigned cpus[2] = {(unsigned)current_cpu,
	    available[0] == (unsigned)current_cpu ? available[1] : available[0]};

	/* Use a worker so that changing affinity does not affect later tests. */
	thd_t thd;
	thd_create(&thd, percpu_arena_migrate_worker, cpus);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	return test(test_thread_arena_resume_percpu,
	    test_thread_arena_follows_cpu);
}
