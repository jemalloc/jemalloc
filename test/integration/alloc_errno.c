#include "test/jemalloc_test.h"

/*
 * A successful allocation must not change errno, even when work it does
 * internally fails, e.g. naming a new mapping on a kernel without
 * CONFIG_ANON_VMA_NAME.  The alloc hook below stands in for such work: it
 * sets errno whenever the arena gets new memory.
 */

static extent_hooks_t   *default_hooks;
static extent_hooks_t    hooks;
static volatile unsigned nallocs;

static void *
alloc_hook(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	void *ret = default_hooks->alloc(
	    default_hooks, new_addr, size, alignment, zero, commit, arena_ind);
	nallocs++;
	set_errno(EINVAL);
	return ret;
}

static void
bind_to_arena_with_hooks(void) {
	size_t sz = sizeof(default_hooks);
	expect_d_eq(mallctl("arena.0.extent_hooks", (void *)&default_hooks, &sz,
	                NULL, 0),
	    0, "Unexpected mallctl() failure");
	hooks = *default_hooks;
	hooks.alloc = alloc_hook;

	unsigned        arena_ind;
	extent_hooks_t *new_hooks = &hooks;
	sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz,
	                (void *)&new_hooks, sizeof(new_hooks)),
	    0, "Unexpected mallctl() failure");
	expect_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena_ind,
	                sizeof(arena_ind)),
	    0, "Unexpected mallctl() failure");
}

TEST_BEGIN(test_alloc_preserves_errno) {
	bind_to_arena_with_hooks();

	/* Large enough to skip the tcache and need new extents. */
	unsigned n = nallocs;
	set_errno(EAGAIN);
	void *p = malloc(4 << 20);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	expect_u_gt(nallocs, n, "malloc() did not get new memory");
	expect_d_eq(get_errno(), EAGAIN, "malloc() changed errno");

	n = nallocs;
	set_errno(EAGAIN);
	p = realloc(p, 64 << 20);
	expect_ptr_not_null(p, "Unexpected realloc() failure");
	expect_u_gt(nallocs, n, "realloc() did not get new memory");
	expect_d_eq(get_errno(), EAGAIN, "realloc() changed errno");

	free(p);
}
TEST_END

int
main(void) {
	return test(test_alloc_preserves_errno);
}
