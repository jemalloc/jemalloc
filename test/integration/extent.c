#include "test/jemalloc_test.h"

#ifdef JEMALLOC_FILL
const char *malloc_conf = "junk:false";
#endif

static extent_hooks_t orig_hooks;
static extent_hooks_t old_hooks;

static bool do_dalloc = true;
static bool do_decommit;

static bool did_alloc;
static bool did_dalloc;
static bool did_commit;
static bool did_decommit;
static bool did_purge;
static bool did_split;
static bool did_merge;

#if 0
#  define TRACE_HOOK(fmt, ...) malloc_printf(fmt, __VA_ARGS__)
#else
#  define TRACE_HOOK(fmt, ...)
#endif

void *
extent_alloc(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit, unsigned arena_ind)
{

	TRACE_HOOK("%s(new_addr=%p, size=%zu, alignment=%zu, *zero=%s, "
	    "*commit=%s, arena_ind=%u)\n", __func__, new_addr, size, alignment,
	    *zero ?  "true" : "false", *commit ? "true" : "false", arena_ind);
	did_alloc = true;
	return (old_hooks.alloc(new_addr, size, alignment, zero, commit,
	    arena_ind));
}

bool
extent_dalloc(void *addr, size_t size, bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(addr=%p, size=%zu, committed=%s, arena_ind=%u)\n",
	    __func__, addr, size, committed ? "true" : "false", arena_ind);
	did_dalloc = true;
	if (!do_dalloc)
		return (true);
	return (old_hooks.dalloc(addr, size, committed, arena_ind));
}

bool
extent_commit(void *addr, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(addr=%p, size=%zu, offset=%zu, length=%zu, "
	    "arena_ind=%u)\n", __func__, addr, size, offset, length,
	    arena_ind);
	err = old_hooks.commit(addr, size, offset, length, arena_ind);
	did_commit = !err;
	return (err);
}

bool
extent_decommit(void *addr, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{
	bool err;

	TRACE_HOOK("%s(addr=%p, size=%zu, offset=%zu, length=%zu, "
	    "arena_ind=%u)\n", __func__, addr, size, offset, length,
	    arena_ind);
	if (!do_decommit)
		return (true);
	err = old_hooks.decommit(addr, size, offset, length, arena_ind);
	did_decommit = !err;
	return (err);
}

bool
extent_purge(void *addr, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	TRACE_HOOK("%s(addr=%p, size=%zu, offset=%zu, length=%zu "
	    "arena_ind=%u)\n", __func__, addr, size, offset, length,
	    arena_ind);
	did_purge = true;
	return (old_hooks.purge(addr, size, offset, length, arena_ind));
}

bool
extent_split(void *addr, size_t size, size_t size_a, size_t size_b,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(addr=%p, size=%zu, size_a=%zu, size_b=%zu, "
	    "committed=%s, arena_ind=%u)\n", __func__, addr, size, size_a,
	    size_b, committed ? "true" : "false", arena_ind);
	did_split = true;
	return (old_hooks.split(addr, size, size_a, size_b, committed,
	    arena_ind));
}

bool
extent_merge(void *addr_a, size_t size_a, void *addr_b, size_t size_b,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(addr_a=%p, size_a=%zu, addr_b=%p size_b=%zu, "
	    "committed=%s, arena_ind=%u)\n", __func__, addr_a, size_a, addr_b,
	    size_b, committed ? "true" : "false", arena_ind);
	did_merge = true;
	return (old_hooks.merge(addr_a, size_a, addr_b, size_b,
	    committed, arena_ind));
}

TEST_BEGIN(test_extent)
{
	void *p;
	size_t old_size, new_size, large0, large1, large2, sz;
	unsigned arena_ind;
	int flags;
	size_t hooks_mib[3], purge_mib[3];
	size_t hooks_miblen, purge_miblen;
	extent_hooks_t new_hooks = {
		extent_alloc,
		extent_dalloc,
		extent_commit,
		extent_decommit,
		extent_purge,
		extent_split,
		extent_merge
	};
	bool xallocx_success_a, xallocx_success_b, xallocx_success_c;

	sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.extend", &arena_ind, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/* Install custom extent hooks. */
	hooks_miblen = sizeof(hooks_mib)/sizeof(size_t);
	assert_d_eq(mallctlnametomib("arena.0.extent_hooks", hooks_mib,
	    &hooks_miblen), 0, "Unexpected mallctlnametomib() failure");
	hooks_mib[1] = (size_t)arena_ind;
	old_size = sizeof(extent_hooks_t);
	new_size = sizeof(extent_hooks_t);
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, &old_hooks, &old_size,
	    &new_hooks, new_size), 0, "Unexpected extent_hooks error");
	orig_hooks = old_hooks;
	assert_ptr_ne(old_hooks.alloc, extent_alloc, "Unexpected alloc error");
	assert_ptr_ne(old_hooks.dalloc, extent_dalloc,
	    "Unexpected dalloc error");
	assert_ptr_ne(old_hooks.commit, extent_commit,
	    "Unexpected commit error");
	assert_ptr_ne(old_hooks.decommit, extent_decommit,
	    "Unexpected decommit error");
	assert_ptr_ne(old_hooks.purge, extent_purge, "Unexpected purge error");
	assert_ptr_ne(old_hooks.split, extent_split, "Unexpected split error");
	assert_ptr_ne(old_hooks.merge, extent_merge, "Unexpected merge error");

	/* Get large size classes. */
	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lextent.0.size", &large0, &sz, NULL, 0), 0,
	    "Unexpected arenas.lextent.0.size failure");
	assert_d_eq(mallctl("arenas.lextent.1.size", &large1, &sz, NULL, 0), 0,
	    "Unexpected arenas.lextent.1.size failure");
	assert_d_eq(mallctl("arenas.lextent.2.size", &large2, &sz, NULL, 0), 0,
	    "Unexpected arenas.lextent.2.size failure");

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
	did_purge = false;
	did_split = false;
	xallocx_success_a = (xallocx(p, large0, 0, flags) == large0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_a) {
		assert_true(did_dalloc, "Expected dalloc");
		assert_false(did_decommit, "Unexpected decommit");
		assert_true(did_purge, "Expected purge");
	}
	assert_true(did_split, "Expected split");
	dallocx(p, flags);
	do_dalloc = true;

	/* Test decommit/commit and observe split/merge. */
	do_dalloc = false;
	do_decommit = true;
	p = mallocx(large0 * 2, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	did_decommit = false;
	did_commit = false;
	did_split = false;
	did_merge = false;
	xallocx_success_b = (xallocx(p, large0, 0, flags) == large0);
	assert_d_eq(mallctlbymib(purge_mib, purge_miblen, NULL, NULL, NULL, 0),
	    0, "Unexpected arena.%u.purge error", arena_ind);
	if (xallocx_success_b)
		assert_true(did_split, "Expected split");
	xallocx_success_c = (xallocx(p, large0 * 2, 0, flags) == large0 * 2);
	assert_b_eq(did_decommit, did_commit, "Expected decommit/commit match");
	if (xallocx_success_b && xallocx_success_c)
		assert_true(did_merge, "Expected merge");
	dallocx(p, flags);
	do_dalloc = true;
	do_decommit = false;

	/* Make sure non-large allocation succeeds. */
	p = mallocx(42, flags);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	dallocx(p, flags);

	/* Restore extent hooks. */
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, NULL, NULL,
	    &old_hooks, new_size), 0, "Unexpected extent_hooks error");
	assert_d_eq(mallctlbymib(hooks_mib, hooks_miblen, &old_hooks, &old_size,
	    NULL, 0), 0, "Unexpected extent_hooks error");
	assert_ptr_eq(old_hooks.alloc, orig_hooks.alloc,
	    "Unexpected alloc error");
	assert_ptr_eq(old_hooks.dalloc, orig_hooks.dalloc,
	    "Unexpected dalloc error");
	assert_ptr_eq(old_hooks.commit, orig_hooks.commit,
	    "Unexpected commit error");
	assert_ptr_eq(old_hooks.decommit, orig_hooks.decommit,
	    "Unexpected decommit error");
	assert_ptr_eq(old_hooks.purge, orig_hooks.purge,
	    "Unexpected purge error");
	assert_ptr_eq(old_hooks.split, orig_hooks.split,
	    "Unexpected split error");
	assert_ptr_eq(old_hooks.merge, orig_hooks.merge,
	    "Unexpected merge error");
}
TEST_END

int
main(void)
{

	return (test(test_extent));
}
