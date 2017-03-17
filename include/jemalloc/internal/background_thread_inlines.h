#ifndef JEMALLOC_INTERNAL_BACKGROUND_THREAD_INLINES_H
#define JEMALLOC_INTERNAL_BACKGROUND_THREAD_INLINES_H

JEMALLOC_ALWAYS_INLINE bool
background_thread_enabled(void) {
	return atomic_load_b(&background_thread_enabled_state, ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE void
background_thread_enabled_set(tsdn_t *tsdn, bool state) {
	malloc_mutex_assert_owner(tsdn, &background_thread_lock);
	atomic_store_b(&background_thread_enabled_state, state, ATOMIC_RELAXED);
}

JEMALLOC_ALWAYS_INLINE background_thread_info_t *
arena_background_thread_info_get(arena_t *arena) {
	unsigned arena_ind = arena_ind_get(arena);
	return &background_thread_info[arena_ind % ncpus];
}

#endif /* JEMALLOC_INTERNAL_BACKGROUND_THREAD_INLINES_H */
