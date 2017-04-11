#ifndef JEMALLOC_INTERNAL_INLINES_B_H
#define JEMALLOC_INTERNAL_INLINES_B_H

#ifndef JEMALLOC_ENABLE_INLINE
extent_t	*iealloc(tsdn_t *tsdn, const void *ptr);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_C_))
/* Choose an arena based on a per-thread value. */
JEMALLOC_INLINE arena_t *
arena_choose_impl(tsd_t *tsd, arena_t *arena, bool internal) {
	arena_t *ret;

	if (arena != NULL) {
		return arena;
	}

	/* During reentrancy, arena 0 is the safest bet. */
	if (*tsd_reentrancy_levelp_get(tsd) > 1) {
		return arena_get(tsd_tsdn(tsd), 0, true);
	}

	ret = internal ? tsd_iarena_get(tsd) : tsd_arena_get(tsd);
	if (unlikely(ret == NULL)) {
		ret = arena_choose_hard(tsd, internal);
		assert(ret);
		if (config_tcache && tcache_available(tsd)) {
			tcache_t *tcache = tcache_get(tsd);
			if (tcache->arena != NULL) {
				/* See comments in tcache_data_init().*/
				assert(tcache->arena ==
				    arena_get(tsd_tsdn(tsd), 0, false));
				if (tcache->arena != ret) {
					tcache_arena_reassociate(tsd_tsdn(tsd),
					    tcache, ret);
				}
			} else {
				tcache_arena_associate(tsd_tsdn(tsd), tcache,
				    ret);
			}
		}
	}

	/*
	 * Note that for percpu arena, if the current arena is outside of the
	 * auto percpu arena range, (i.e. thread is assigned to a manually
	 * managed arena), then percpu arena is skipped.
	 */
	if (have_percpu_arena && (percpu_arena_mode != percpu_arena_disabled) &&
	    (arena_ind_get(ret) < percpu_arena_ind_limit()) &&
	    (ret->last_thd != tsd_tsdn(tsd))) {
		unsigned ind = percpu_arena_choose();
		if (arena_ind_get(ret) != ind) {
			percpu_arena_update(tsd, ind);
			ret = tsd_arena_get(tsd);
		}
		ret->last_thd = tsd_tsdn(tsd);
	}

	return ret;
}

JEMALLOC_INLINE arena_t *
arena_choose(tsd_t *tsd, arena_t *arena) {
	return arena_choose_impl(tsd, arena, false);
}

JEMALLOC_INLINE arena_t *
arena_ichoose(tsd_t *tsd, arena_t *arena) {
	return arena_choose_impl(tsd, arena, true);
}

JEMALLOC_ALWAYS_INLINE extent_t *
iealloc(tsdn_t *tsdn, const void *ptr) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	return rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true);
}
#endif

#endif /* JEMALLOC_INTERNAL_INLINES_B_H */
