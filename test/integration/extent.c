#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
const char *malloc_conf = "junk:false";
#endif

static void	*extent_alloc(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);
static bool	extent_dalloc(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static bool	extent_commit(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_decommit(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_lazy(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_forced(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_split(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t size_a, size_t size_b, bool committed,
    unsigned arena_ind);
static bool	extent_merge(extent_hooks_t *extent_hooks, void *addr_a,
    size_t size_a, void *addr_b, size_t size_b, bool committed,
    unsigned arena_ind);

static extent_hooks_t hooks = {
	extent_alloc,
	extent_dalloc,
	extent_commit,
	extent_decommit,
	extent_purge_lazy,
	extent_purge_forced,
	extent_split,
	extent_merge
};
static extent_hooks_t *new_hooks = &hooks;
static extent_hooks_t *orig_hooks;
static extent_hooks_t *old_hooks;

static bool do_dalloc = true;
static bool do_decommit;

static bool did_alloc;
static bool did_dalloc;
static bool did_commit;
static bool did_decommit;
static bool did_purge_lazy;
static bool did_purge_forced;
static bool tried_split;
static bool did_split;
static bool did_merge;

#if 0
#  define TRACE_HOOK(fmt, ...) malloc_printf(fmt, __VA_ARGS__)
#else
#  define TRACE_HOOK(fmt, ...)
#endif

static void *
extent_alloc(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, new_addr=%p, size=%zu, alignment=%zu, "
	    "*zero=%s, *commit=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    new_addr, size, alignment, *zero ?  "true" : "false", *commit ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->alloc, extent_alloc, "Wrong hook function");
	did_alloc = true;
	return (old_hooks->alloc(old_hooks, new_addr, size, alignment, zero,
	    commit, 0));
}

static bool
extent_dalloc(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, committed=%s, "
	    "arena_ind=%u)\n", __func__, extent_hooks, addr, size, committed ?
	    "true" : "false", arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->dalloc, extent_dalloc,
	    "Wrong hook function");
	did_dalloc = true;
	if (!do_dalloc)
		return (true);
	return (old_hooks->dalloc(old_hooks, addr, size, committed, 0));
}

static bool
extent_commit(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu, arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->commit, extent_commit,
	    "Wrong hook function");
	err = old_hooks->commit(old_hooks, addr, size, offset, length, 0);
	did_commit = !err;
	return (err);
}

static bool
extent_decommit(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu, arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->decommit, extent_decommit,
	    "Wrong hook function");
	if (!do_decommit)
		return (true);
	err = old_hooks->decommit(old_hooks, addr, size, offset, length, 0);
	did_decommit = !err;
	return (err);
}

static bool
extent_purge_lazy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->purge_lazy, extent_purge_lazy,
	    "Wrong hook function");
	did_purge_lazy = true;
	return (old_hooks->purge_lazy == NULL ||
	    old_hooks->purge_lazy(old_hooks, addr, size, offset, length, 0));
}

static bool
extent_purge_forced(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->purge_forced, extent_purge_forced,
	    "Wrong hook function");
	did_purge_forced = true;
	return (old_hooks->purge_forced == NULL ||
	    old_hooks->purge_forced(old_hooks, addr, size, offset, length, 0));
}

static bool
extent_split(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, size_a=%zu, "
	    "size_b=%zu, committed=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    addr, size, size_a, size_b, committed ? "true" : "false",
	    arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->split, extent_split, "Wrong hook function");
	tried_split = true;
	err = (old_hooks->split == NULL || old_hooks->split(old_hooks, addr,
	    size, size_a, size_b, committed, 0));
	did_split = !err;
	return (err);
}

static bool
extent_merge(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(extent_hooks=%p, addr_a=%p, size_a=%zu, addr_b=%p "
	    "size_b=%zu, committed=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    addr_a, size_a, addr_b, size_b, committed ? "true" : "false",
	    arena_ind);
	assert_ptr_eq(extent_hooks, new_hooks,
	    "extent_hooks should be same as pointer used to set hooks");
	assert_ptr_eq(extent_hooks->merge, extent_merge, "Wrong hook function");
	err = (old_hooks->merge == NULL || old_hooks->merge(old_hooks, addr_a,
	    size_a, addr_b, size_b, committed, 0));
	did_merge = !err;
	return (err);
}

static void
test_extent_body(unsigned arena_ind)
{
	void *p;
	size_t large0, large1, large2, sz;
	size_t purge_mib[3];
	size_t purge_miblen;
	int flags;
	bool xallocx_success_a, xallocx_success_b, xallocx_success_c;

	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Get large size classes. */
	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lextent.0.size", (void *)&large0, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.0.size failure");
	assert_d_eq(mallctl("arenas.lextent.1.size", (void *)&large1, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.1.size failure");
	assert_d_eq(mallctl("arenas.lextent.2.size", (void *)&large2, &sz, NULL,
	    0), 0, "Unexpected arenas.lextent.2.size failure");

	/* Test dalloc/decommit/purge cascade. */
	purge_miblen = sizeof(purge_mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.purge", purge_mib, &purge_miblen),
	    0, "Unexpected mallctlnametomib() failure");
	purge_mib[1] = (size_t)arena_ind;
	do_dalloc = false;
	do_decommit = false;
	p = mallocx(large0 * 2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	did_dalloc = false;
	did_decommit = false;
	did_purge_lazy = false;
	did_purge_forced = false;
	tried_split = false;
	did_split = false;
	xallocx_success_a = (xallocx(p, large0, 0, flags) == large0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_a) {
		assert_true(did_dalloc, "Expected dalloc");
		assert_false(did_decommit, "Unexpected decommit");
		assert_true(did_purge_lazy || did_purge_forced,
		    "Expected purge");
	}
	assert_true(tried_split, "Expected split");
	dallocx(p, flags);
	do_dalloc = true;

	/* Test decommit/commit and observe split/merge. */
	do_dalloc = false;
	do_decommit = true;
	p = mallocx(large0 * 2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	did_decommit = false;
	did_commit = false;
	tried_split = false;
	did_split = false;
	did_merge = false;
	xallocx_success_b = (xallocx(p, large0, 0, flags) == large0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_b)
		assert_true(did_split, "Expected split");
	xallocx_success_c = (xallocx(p, large0 * 2, 0, flags) == large0 * 2);
	if (did_split) {
		assert_b_eq(did_decommit, did_commit,
		    "Expected decommit/commit match");
	}
	if (xallocx_success_b && xallocx_success_c)
		assert_true(did_merge, "Expected merge");
	dallocx(p, flags);
	do_dalloc = true;
	do_decommit = false;

	/* Make sure non-large allocation succeeds. */
	p = mallocx(42, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	dallocx(p, flags);
}

TEST_BEGIN(test_extent_manual_hook)
{
	unsigned arena_ind;
	size_t old_size, new_size, sz;
	size_t hooks_mib[3];
	size_t hooks_miblen;

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.extend", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	/* Install custom extent hooks. */
	hooks_miblen = sizeof(hooks_mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.extent_hooks", hooks_mib,
	    &hooks_miblen), 0, "Unexpected mallctlnametomib() failure");
	hooks_mib[1] = (size_t)arena_ind;
	old_size = sizeof(extent_hooks_t *);
	new_size = sizeof(extent_hooks_t *);
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, (void *)&new_hooks, new_size), 0,
	    "Unexpected extent_hooks error");
	orig_hooks = old_hooks;
	assert_ptr_ne(old_hooks->alloc, extent_alloc, "Unexpected alloc error");
	assert_ptr_ne(old_hooks->dalloc, extent_dalloc,
	    "Unexpected dalloc error");
	assert_ptr_ne(old_hooks->commit, extent_commit,
	    "Unexpected commit error");
	assert_ptr_ne(old_hooks->decommit, extent_decommit,
	    "Unexpected decommit error");
	assert_ptr_ne(old_hooks->purge_lazy, extent_purge_lazy,
	    "Unexpected purge_lazy error");
	assert_ptr_ne(old_hooks->purge_forced, extent_purge_forced,
	    "Unexpected purge_forced error");
	assert_ptr_ne(old_hooks->split, extent_split, "Unexpected split error");
	assert_ptr_ne(old_hooks->merge, extent_merge, "Unexpected merge error");

	test_extent_body(arena_ind);

	/* Restore extent hooks. */
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, NULL, NULL,
	    (void *)&old_hooks, new_size), 0, "Unexpected extent_hooks error");
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, (void *)&old_hooks,
	    &old_size, NULL, 0), 0, "Unexpected extent_hooks error");
	assert_ptr_eq(old_hooks, orig_hooks, "Unexpected hooks error");
	assert_ptr_eq(old_hooks->alloc, orig_hooks->alloc,
	    "Unexpected alloc error");
	assert_ptr_eq(old_hooks->dalloc, orig_hooks->dalloc,
	    "Unexpected dalloc error");
	assert_ptr_eq(old_hooks->commit, orig_hooks->commit,
	    "Unexpected commit error");
	assert_ptr_eq(old_hooks->decommit, orig_hooks->decommit,
	    "Unexpected decommit error");
	assert_ptr_eq(old_hooks->purge_lazy, orig_hooks->purge_lazy,
	    "Unexpected purge_lazy error");
	assert_ptr_eq(old_hooks->purge_forced, orig_hooks->purge_forced,
	    "Unexpected purge_forced error");
	assert_ptr_eq(old_hooks->split, orig_hooks->split,
	    "Unexpected split error");
	assert_ptr_eq(old_hooks->merge, orig_hooks->merge,
	    "Unexpected merge error");
}
TEST_END

TEST_BEGIN(test_extent_auto_hook)
{
	unsigned arena_ind;
	size_t new_size, sz;

	sz = sizeof(unsigned);
	new_size = sizeof(extent_hooks_t *);
	assert_d_eq(mallctl("arenas.extend", (void *)&arena_ind, &sz,
	    (void *)&new_hooks, new_size), 0, "Unexpected mallctl() failure");

	test_extent_body(arena_ind);
}
TEST_END

int
main(void)
{

	return (test(
	    test_extent_manual_hook,
	    test_extent_auto_hook));
}
