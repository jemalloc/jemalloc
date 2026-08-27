#include "test/jemalloc_test.h"

#ifdef JEMALLOC_PERCPU_ARENA
const char *malloc_conf = "percpu_arena:percpu";
#endif

/*
 * Under percpu_arena, binding a thread to a manual arena (an index at or above
 * the per-CPU auto range) is one-way: percpu never reclaims it (see
 * arena_choose_impl). Setting thread.arena back to an index in the auto range
 * resumes per-CPU selection instead of failing with EPERM.
 */
TEST_BEGIN(test_thread_arena_resume_percpu) {
	test_skip_if(!have_percpu_arena
	    || !PERCPU_ARENA_ENABLED(opt_percpu_arena));

	unsigned limit = percpu_arena_ind_limit(opt_percpu_arena);
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

int
main(void) {
	return test(
	    test_thread_arena_resume_percpu);
}
