#ifndef JEMALLOC_INTERNAL_INLINES_A_H
#define JEMALLOC_INTERNAL_INLINES_A_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/os.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/tcache.h"
#include "jemalloc/internal/ticker.h"

static inline arena_t *
arena_get(tsdn_t *tsdn, unsigned ind, bool init_if_missing) {
	arena_t *ret;

	assert(ind < MALLOCX_ARENA_LIMIT);

	ret = (arena_t *)atomic_load_p(&arenas[ind], ATOMIC_ACQUIRE);
	if (unlikely(ret == NULL)) {
		if (init_if_missing) {
			ret = arena_init(tsdn, ind, &arena_config_default);
		}
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE bool
tcache_available(tsd_t *tsd) {
	/*
	 * Thread specific auto tcache might be unavailable if: 1) during tcache
	 * initialization, or 2) disabled through thread.tcache.enabled mallctl
	 * or config options.  This check covers all cases.
	 */
	if (likely(tsd_tcache_enabled_get(tsd))) {
		/* Associated arena == NULL implies tcache init in progress. */
		if (config_debug && tsd_tcache_slowp_get(tsd)->arena != NULL) {
			tcache_assert_initialized(tsd_tcachep_get(tsd));
		}
		return true;
	}

	return false;
}

JEMALLOC_ALWAYS_INLINE tcache_t *
tcache_get(tsd_t *tsd) {
	if (!tcache_available(tsd)) {
		return NULL;
	}

	return tsd_tcachep_get(tsd);
}

JEMALLOC_ALWAYS_INLINE tcache_slow_t *
tcache_slow_get(tsd_t *tsd) {
	if (!tcache_available(tsd)) {
		return NULL;
	}

	return tsd_tcache_slowp_get(tsd);
}

static inline void
pre_reentrancy(tsd_t *tsd, arena_t *arena) {
	/* arena is the current context.  Reentry from a0 is not allowed. */
	assert(arena != arena_get(tsd_tsdn(tsd), 0, false));
	tsd_pre_reentrancy_raw(tsd);
}

static inline void
post_reentrancy(tsd_t *tsd) {
	tsd_post_reentrancy_raw(tsd);
}

#endif /* JEMALLOC_INTERNAL_INLINES_A_H */
