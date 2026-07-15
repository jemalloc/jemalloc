#include "test/jemalloc_test.h"

/*
 * When percpu_arena is enabled a thread is bound to a manually created arena
 * (an index at or above the per-CPU auto range) to route a bounded region of
 * work to a dedicated arena, then handed back to automatic per-CPU selection.
 *
 * Setting thread.arena to an index within the per-CPU auto range is the signal
 * to resume per-CPU management. Without it a thread bound to a manual arena is
 * never reclaimed by percpu (see arena_choose_impl), so it would stay pinned to
 * the manual arena forever and its later allocations would land there instead
 * of following the CPU.
 */
TEST_BEGIN(test_thread_arena_resume_percpu) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena));

	unsigned limit = percpu_arena_ind_limit(opt_percpu_arena);

	/* Force the thread to be bound to its current CPU's arena. */
	void *p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	unsigned cur;
	size_t sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit,
	    "Thread should start bound to a per-CPU (auto) arena");

	/* Create a dedicated arena, which is always outside the auto range. */
	unsigned manual;
	sz = sizeof(manual);
	expect_d_eq(mallctl("arenas.create", (void *)&manual, &sz, NULL, 0), 0,
	    "Unexpected arenas.create() failure");
	expect_u_ge(manual, limit,
	    "A manually created arena must be outside the per-CPU range");

	/* Binding to a manual arena is allowed and takes effect. */
	unsigned old;
	sz = sizeof(old);
	expect_d_eq(mallctl("thread.arena", (void *)&old, &sz, (void *)&manual,
	    sizeof(manual)), 0,
	    "Binding to a manual arena should be allowed under percpu");
	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_eq(cur, manual, "Thread should be bound to the manual arena");

	/*
	 * Setting thread.arena to an index in the per-CPU range resumes
	 * automatic per-CPU selection instead of returning EPERM.
	 */
	unsigned resume = 0;
	expect_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&resume,
	    sizeof(resume)), 0,
	    "Setting thread.arena within the per-CPU range should resume "
	    "per-CPU selection, not fail");

	sz = sizeof(cur);
	expect_d_eq(mallctl("thread.arena", (void *)&cur, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_u_lt(cur, limit,
	    "Thread should be back under per-CPU management after resuming");

	dallocx(p, 0);
}
TEST_END

int
main(void) {
	return test(
	    test_thread_arena_resume_percpu);
}
