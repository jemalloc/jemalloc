#ifndef JEMALLOC_INTERNAL_EHOOKS_H
#define JEMALLOC_INTERNAL_EHOOKS_H

#include "jemalloc/internal/atomic.h"

extern const extent_hooks_t extent_hooks_default;

typedef struct ehooks_s ehooks_t;
struct ehooks_s {
	/* Logically an extent_hooks_t *. */
	atomic_p_t ptr;
};

/* NOT PUBLIC. */
void *ehooks_default_alloc_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind);
void *ehooks_default_alloc(extent_hooks_t *extent_hooks, void *new_addr,
    size_t size, size_t alignment, bool *zero, bool *commit,
    unsigned arena_ind);

bool ehooks_default_dalloc_impl(void *addr, size_t size);
bool ehooks_default_dalloc(extent_hooks_t *extent_hooks, void *addr,
    size_t size, bool committed, unsigned arena_ind);

static inline void
ehooks_pre_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_pre_reentrancy_raw(tsd);
}

static inline void
ehooks_post_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_post_reentrancy_raw(tsd);
}

/* PUBLIC. */
void ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks);

static inline void
ehooks_set_extent_hooks_ptr(ehooks_t *ehooks, extent_hooks_t *extent_hooks) {
	atomic_store_p(&ehooks->ptr, extent_hooks, ATOMIC_RELEASE);
}

static inline extent_hooks_t *
ehooks_get_extent_hooks_ptr(ehooks_t *ehooks) {
	return (extent_hooks_t *)atomic_load_p(&ehooks->ptr, ATOMIC_ACQUIRE);
}

static inline bool
ehooks_are_default(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks) == &extent_hooks_default;
}

static inline bool
ehooks_destroy_is_noop(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->destroy == NULL;
}

static inline bool
ehooks_purge_lazy_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->purge_lazy == NULL;
}

static inline bool
ehooks_purge_forced_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->purge_forced == NULL;
}

static inline bool
ehooks_split_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->split == NULL;
}

static inline bool
ehooks_merge_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->merge == NULL;
}

static inline void *
ehooks_alloc(tsdn_t *tsdn, ehooks_t *ehooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &extent_hooks_default) {
		return ehooks_default_alloc_impl(tsdn, new_addr, size,
		    alignment, zero, commit, arena_ind);
	}
	ehooks_pre_reentrancy(tsdn);
	void *ret = extent_hooks->alloc(extent_hooks, new_addr, size, alignment,
	    zero, commit, arena_ind);
	ehooks_post_reentrancy(tsdn);
	return ret;
}

static inline bool
ehooks_dalloc(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &extent_hooks_default) {
		return ehooks_default_dalloc_impl(addr, size);
	} else if (extent_hooks->dalloc == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->dalloc(extent_hooks, addr, size,
		    committed, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline void
ehooks_destroy(ehooks_t *ehooks, void *addr, size_t size, bool committed,
    unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->destroy == NULL) {
		return;
	}
	extent_hooks->destroy(extent_hooks, addr, size, committed, arena_ind);
}

static inline bool
ehooks_commit(ehooks_t *ehooks, void *addr, size_t size, size_t offset,
    size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->commit == NULL) {
		return true;
	}
	return extent_hooks->commit(extent_hooks, addr, size, offset, length,
	    arena_ind);
}

static inline bool
ehooks_decommit(ehooks_t *ehooks, void *addr, size_t size, size_t offset,
    size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->decommit == NULL) {
		return true;
	}
	return extent_hooks->decommit(extent_hooks, addr, size, offset, length,
	    arena_ind);
}

static inline bool
ehooks_purge_lazy(ehooks_t *ehooks, void *addr, size_t size, size_t offset,
    size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->purge_lazy == NULL) {
		return true;
	}
	return extent_hooks->purge_lazy(extent_hooks, addr, size, offset,
	    length, arena_ind);
}

static inline bool
ehooks_purge_forced(ehooks_t *ehooks, void *addr, size_t size, size_t offset,
    size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->purge_forced == NULL) {
		return true;
	}
	return extent_hooks->purge_forced(extent_hooks, addr, size, offset,
	    length, arena_ind);
}

static inline bool
ehooks_split(ehooks_t *ehooks, void *addr, size_t size, size_t size_a,
    size_t size_b, bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->split == NULL) {
		return true;
	}
	return extent_hooks->split(extent_hooks, addr, size, size_a, size_b,
	    committed, arena_ind);
}

static inline bool
ehooks_merge(ehooks_t *ehooks, void *addr_a, size_t size_a, void *addr_b,
    size_t size_b, bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks->merge == NULL) {
		return true;
	}
	return extent_hooks->merge(extent_hooks, addr_a, size_a, addr_b, size_b,
	    committed, arena_ind);
}

#endif /* JEMALLOC_INTERNAL_EHOOKS_H */
