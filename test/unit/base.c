#include "test/jemalloc_test.h"

static void	*extent_alloc_hook(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);
static bool	extent_dalloc_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);
static bool	extent_decommit_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_lazy_hook(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind);
static bool	extent_purge_forced_hook(extent_hooks_t *extent_hooks,
    void *addr, size_t size, size_t offset, size_t length, unsigned arena_ind);

static extent_hooks_t hooks_not_null = {
	extent_alloc_hook,
	extent_dalloc_hook,
	NULL, /* commit */
	extent_decommit_hook,
	extent_purge_lazy_hook,
	extent_purge_forced_hook,
	NULL, /* split */
	NULL /* merge */
};

static extent_hooks_t hooks_null = {
	extent_alloc_hook,
	NULL, /* dalloc */
	NULL, /* commit */
	NULL, /* decommit */
	NULL, /* purge_lazy */
	NULL, /* purge_forced */
	NULL, /* split */
	NULL /* merge */
};

static bool	did_alloc;
static bool	did_dalloc;
static bool	did_decommit;
static bool	did_purge_lazy;
static bool	did_purge_forced;

#if 0
#  define TRACE_HOOK(fmt, ...) malloc_printf(fmt, __VA_ARGS__)
#else
#  define TRACE_HOOK(fmt, ...)
#endif

static void *
extent_alloc_hook(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, new_addr=%p, size=%zu, alignment=%zu, "
	    "*zero=%s, *commit=%s, arena_ind=%u)\n", __func__, extent_hooks,
	    new_addr, size, alignment, *zero ?  "true" : "false", *commit ?
	    "true" : "false", arena_ind);
	did_alloc = true;
	return (extent_hooks_default.alloc(
	    (extent_hooks_t *)&extent_hooks_default, new_addr, size, alignment,
	    zero, commit, 0));
}

static bool
extent_dalloc_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, committed=%s, "
	    "arena_ind=%u)\n", __func__, extent_hooks, addr, size, committed ?
	    "true" : "false", arena_ind);
	did_dalloc = true;
	return (true); /* Cause cascade. */
}

static bool
extent_decommit_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu, arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	did_decommit = true;
	return (true); /* Cause cascade. */
}

static bool
extent_purge_lazy_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	did_purge_lazy = true;
	return (true); /* Cause cascade. */
}

static bool
extent_purge_forced_hook(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind)
{

	TRACE_HOOK("%s(extent_hooks=%p, addr=%p, size=%zu, offset=%zu, "
	    "length=%zu arena_ind=%u)\n", __func__, extent_hooks, addr, size,
	    offset, length, arena_ind);
	did_purge_forced = true;
	return (true); /* Cause cascade. */
}

TEST_BEGIN(test_base_hooks_default)
{
	tsdn_t *tsdn;
	base_t *base;
	size_t allocated0, allocated1, resident, mapped;

	tsdn = tsdn_fetch();
	base = base_new(tsdn, 0, (extent_hooks_t *)&extent_hooks_default);

	base_stats_get(tsdn, base, &allocated0, &resident, &mapped);
	assert_zu_ge(allocated0, sizeof(base_t),
	    "Base header should count as allocated");

	assert_ptr_not_null(base_alloc(tsdn, base, 42, 1),
	    "Unexpected base_alloc() failure");

	base_stats_get(tsdn, base, &allocated1, &resident, &mapped);
	assert_zu_ge(allocated1 - allocated0, 42,
	    "At least 42 bytes were allocated by base_alloc()");

	base_delete(base);
}
TEST_END

TEST_BEGIN(test_base_hooks_null)
{
	tsdn_t *tsdn;
	base_t *base;
	size_t allocated0, allocated1, resident, mapped;

	tsdn = tsdn_fetch();
	base = base_new(tsdn, 0, (extent_hooks_t *)&hooks_null);
	assert_ptr_not_null(base, "Unexpected base_new() failure");

	base_stats_get(tsdn, base, &allocated0, &resident, &mapped);
	assert_zu_ge(allocated0, sizeof(base_t),
	    "Base header should count as allocated");

	assert_ptr_not_null(base_alloc(tsdn, base, 42, 1),
	    "Unexpected base_alloc() failure");

	base_stats_get(tsdn, base, &allocated1, &resident, &mapped);
	assert_zu_ge(allocated1 - allocated0, 42,
	    "At least 42 bytes were allocated by base_alloc()");

	base_delete(base);
}
TEST_END

TEST_BEGIN(test_base_hooks_not_null)
{
	tsdn_t *tsdn;
	base_t *base;
	void *p, *q, *r, *r_exp;

	tsdn = tsdn_fetch();
	did_alloc = false;
	base = base_new(tsdn, 0, (extent_hooks_t *)&hooks_not_null);
	assert_ptr_not_null(base, "Unexpected base_new() failure");
	assert_true(did_alloc, "Expected alloc hook call");

	/*
	 * Check for tight packing at specified alignment under simple
	 * conditions.
	 */
	{
		const size_t alignments[] = {
			1,
			QUANTUM,
			QUANTUM << 1,
			CACHELINE,
			CACHELINE << 1,
		};
		unsigned i;

		for (i = 0; i < sizeof(alignments) / sizeof(size_t); i++) {
			size_t alignment = alignments[i];
			size_t align_ceil = ALIGNMENT_CEILING(alignment,
			    QUANTUM);
			p = base_alloc(tsdn, base, 1, alignment);
			assert_ptr_not_null(p,
			    "Unexpected base_alloc() failure");
			assert_ptr_eq(p,
			    (void *)(ALIGNMENT_CEILING((uintptr_t)p,
			    alignment)), "Expected quantum alignment");
			q = base_alloc(tsdn, base, alignment, alignment);
			assert_ptr_not_null(q,
			    "Unexpected base_alloc() failure");
			assert_ptr_eq((void *)((uintptr_t)p + align_ceil), q,
			    "Minimal allocation should take up %zu bytes",
			    align_ceil);
			r = base_alloc(tsdn, base, 1, alignment);
			assert_ptr_not_null(r,
			    "Unexpected base_alloc() failure");
			assert_ptr_eq((void *)((uintptr_t)q + align_ceil), r,
			    "Minimal allocation should take up %zu bytes",
			    align_ceil);
		}
	}

	/*
	 * Allocate an object that cannot fit in the first block, then verify
	 * that the first block's remaining space is considered for subsequent
	 * allocation.
	 */
	assert_zu_ge(extent_size_get(&base->blocks->extent), QUANTUM,
	    "Remainder insufficient for test");
	/* Use up all but one quantum of block. */
	while (extent_size_get(&base->blocks->extent) > QUANTUM) {
		p = base_alloc(tsdn, base, QUANTUM, QUANTUM);
		assert_ptr_not_null(p, "Unexpected base_alloc() failure");
	}
	r_exp = extent_addr_get(&base->blocks->extent);
	assert_zu_eq(base->extent_sn_next, 1, "One extant block expected");
	q = base_alloc(tsdn, base, QUANTUM + 1, QUANTUM);
	assert_ptr_not_null(q, "Unexpected base_alloc() failure");
	assert_ptr_ne(q, r_exp, "Expected allocation from new block");
	assert_zu_eq(base->extent_sn_next, 2, "Two extant blocks expected");
	r = base_alloc(tsdn, base, QUANTUM, QUANTUM);
	assert_ptr_not_null(r, "Unexpected base_alloc() failure");
	assert_ptr_eq(r, r_exp, "Expected allocation from first block");
	assert_zu_eq(base->extent_sn_next, 2, "Two extant blocks expected");

	/*
	 * Check for proper alignment support when normal blocks are too small.
	 */
	{
		const size_t alignments[] = {
			HUGEPAGE,
			HUGEPAGE << 1
		};
		unsigned i;

		for (i = 0; i < sizeof(alignments) / sizeof(size_t); i++) {
			size_t alignment = alignments[i];
			p = base_alloc(tsdn, base, QUANTUM, alignment);
			assert_ptr_not_null(p,
			    "Unexpected base_alloc() failure");
			assert_ptr_eq(p,
			    (void *)(ALIGNMENT_CEILING((uintptr_t)p,
			    alignment)), "Expected %zu-byte alignment",
			    alignment);
		}
	}

	did_dalloc = did_decommit = did_purge_lazy = did_purge_forced = false;
	base_delete(base);
	assert_true(did_dalloc, "Expected dalloc hook call");
	assert_true(did_decommit, "Expected decommit hook call");
	assert_true(did_purge_lazy, "Expected purge_lazy hook call");
	assert_true(did_purge_forced, "Expected purge_forced hook call");
}
TEST_END

int
main(void)
{

	return (test(
	    test_base_hooks_default,
	    test_base_hooks_null,
	    test_base_hooks_not_null));
}
