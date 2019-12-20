#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/extent_mmap.h"

void
ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks, unsigned ind) {
	/* All other hooks are optional; this one is not. */
	assert(extent_hooks->alloc != NULL);
	ehooks->ind = ind;
	ehooks_set_extent_hooks_ptr(ehooks, extent_hooks);
}

/*
 * If the caller specifies (!*zero), it is still possible to receive zeroed
 * memory, in which case *zero is toggled to true.  arena_extent_alloc() takes
 * advantage of this to avoid demanding zeroed extents, but taking advantage of
 * them if they are returned.
 */
static void *
extent_alloc_core(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec) {
	void *ret;

	assert(size != 0);
	assert(alignment != 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}
	/* mmap. */
	if ((ret = extent_alloc_mmap(new_addr, size, alignment, zero, commit))
	    != NULL) {
		return ret;
	}
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    extent_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL) {
		return ret;
	}

	/* All strategies for allocation failed. */
	return NULL;
}

void *
ehooks_default_alloc_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	arena_t *arena = arena_get(tsdn, arena_ind, false);
	void *ret = extent_alloc_core(tsdn, arena, new_addr, size, alignment, zero,
	    commit, (dss_prec_t)atomic_load_u(&arena->dss_prec,
	    ATOMIC_RELAXED));
	if (have_madvise_huge && ret) {
		pages_set_thp_state(ret, size);
	}
	return ret;
}

static void *
ehooks_default_alloc(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	tsdn_t *tsdn;
	arena_t *arena;

	tsdn = tsdn_fetch();
	arena = arena_get(tsdn, arena_ind, false);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);

	return ehooks_default_alloc_impl(tsdn, new_addr, size,
	    ALIGNMENT_CEILING(alignment, PAGE), zero, commit,
	    arena_ind_get(arena));
}

bool
ehooks_default_dalloc_impl(void *addr, size_t size) {
	if (!have_dss || !extent_in_dss(addr)) {
		return extent_dalloc_mmap(addr, size);
	}
	return true;
}

static bool
ehooks_default_dalloc(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	return ehooks_default_dalloc_impl(addr, size);
}

void
ehooks_default_destroy_impl(void *addr, size_t size) {
	if (!have_dss || !extent_in_dss(addr)) {
		pages_unmap(addr, size);
	}
}

static void
ehooks_default_destroy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	ehooks_default_destroy_impl(addr, size);
}

bool
ehooks_default_commit_impl(void *addr, size_t offset, size_t length) {
	return pages_commit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}

static bool
ehooks_default_commit(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	return ehooks_default_commit_impl(addr, offset, length);
}

bool
ehooks_default_decommit_impl(void *addr, size_t offset, size_t length) {
	return pages_decommit((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}

static bool
ehooks_default_decommit(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	return ehooks_default_decommit_impl(addr, offset, length);
}

#ifdef PAGES_CAN_PURGE_LAZY
bool
ehooks_default_purge_lazy_impl(void *addr, size_t offset, size_t length) {
	return pages_purge_lazy((void *)((uintptr_t)addr + (uintptr_t)offset),
	    length);
}

static bool
ehooks_default_purge_lazy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);
	return ehooks_default_purge_lazy_impl(addr, offset, length);
}
#endif

#ifdef PAGES_CAN_PURGE_FORCED
bool
ehooks_default_purge_forced_impl(void *addr, size_t offset, size_t length) {
	return pages_purge_forced((void *)((uintptr_t)addr +
	    (uintptr_t)offset), length);
}

static bool
ehooks_default_purge_forced(extent_hooks_t *extent_hooks, void *addr,
    size_t size, size_t offset, size_t length, unsigned arena_ind) {
	assert(addr != NULL);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);
	return ehooks_default_purge_forced_impl(addr, offset, length);
}
#endif

bool
ehooks_default_split_impl() {
	if (!maps_coalesce) {
		/*
		 * Without retain, only whole regions can be purged (required by
		 * MEM_RELEASE on Windows) -- therefore disallow splitting.  See
		 * comments in extent_head_no_merge().
		 */
		return !opt_retain;
	}

	return false;
}

static bool
ehooks_default_split(extent_hooks_t *extent_hooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	return ehooks_default_split_impl();
}

static inline bool
ehooks_same_sn(tsdn_t *tsdn, void *addr_a, void *addr_b) {
	edata_t *a = iealloc(tsdn, addr_a);
	edata_t *b = iealloc(tsdn, addr_b);
	return edata_sn_comp(a, b) == 0;
}

/*
 * Returns true if the given extents can't be merged because of their head bit
 * settings.  Assumes the second extent has the higher address.
 */
static bool
ehooks_no_merge_heads(tsdn_t *tsdn, void *addr_a, bool head_a, void *addr_b,
    bool head_b) {
	/*
	 * When coalesce is not always allowed (Windows), only merge extents
	 * from the same VirtualAlloc region under opt.retain (in which case
	 * MEM_DECOMMIT is utilized for purging).
	 */
	if (maps_coalesce) {
		return false;
	}
	if (!opt_retain) {
		return true;
	}
	/* If b is a head extent, disallow the cross-region merge. */
	if (head_b) {
		/*
		 * Additionally, sn should not overflow with retain; sanity
		 * check that different regions have unique sn.
		 */
		assert(!ehooks_same_sn(tsdn, addr_a, addr_b));
		return true;
	}
	assert(ehooks_same_sn(tsdn, addr_a, addr_b));

	return false;
}

bool
ehooks_default_merge_impl(tsdn_t *tsdn, void *addr_a, bool head_a, void *addr_b,
    bool head_b) {
	if (ehooks_no_merge_heads(tsdn, addr_a, head_a, addr_b, head_b)) {
		return true;
	}
	if (!maps_coalesce && !opt_retain) {
		return true;
	}
	if (have_dss && !extent_dss_mergeable(addr_a, addr_b)) {
		return true;
	}

	return false;
}

bool
ehooks_default_merge(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	tsdn_t *tsdn = tsdn_fetch();
	edata_t *a = iealloc(tsdn, addr_a);
	bool head_a = edata_is_head_get(a);
	edata_t *b = iealloc(tsdn, addr_b);
	bool head_b = edata_is_head_get(b);
	return ehooks_default_merge_impl(tsdn, addr_a, head_a, addr_b, head_b);
}

void
ehooks_default_zero_impl(void *addr, size_t size) {
	/*
	 * By default, we try to zero out memory using OS-provided demand-zeroed
	 * pages.  If the user has specifically requested hugepages, though, we
	 * don't want to purge in the middle of a hugepage (which would break it
	 * up), so we act conservatively and use memset.
	 */
	bool needs_memset = true;
	if (opt_thp != thp_mode_always) {
		needs_memset = pages_purge_forced(addr, size);
	}
	if (needs_memset) {
		memset(addr, 0, size);
	}
}

const extent_hooks_t ehooks_default_extent_hooks = {
	ehooks_default_alloc,
	ehooks_default_dalloc,
	ehooks_default_destroy,
	ehooks_default_commit,
	ehooks_default_decommit,
#ifdef PAGES_CAN_PURGE_LAZY
	ehooks_default_purge_lazy,
#else
	NULL,
#endif
#ifdef PAGES_CAN_PURGE_FORCED
	ehooks_default_purge_forced,
#else
	NULL,
#endif
	ehooks_default_split,
	ehooks_default_merge
};
