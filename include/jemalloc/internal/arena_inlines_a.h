#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_A_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_A_H

#ifndef JEMALLOC_ENABLE_INLINE
unsigned	arena_ind_get(const arena_t *arena);
void	arena_internal_add(arena_t *arena, size_t size);
void	arena_internal_sub(arena_t *arena, size_t size);
size_t	arena_internal_get(arena_t *arena);
bool	arena_prof_accum(tsdn_t *tsdn, arena_t *arena, uint64_t accumbytes);
void	percpu_arena_update(tsd_t *tsd, unsigned cpu);
#endif /* JEMALLOC_ENABLE_INLINE */

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ARENA_C_))

JEMALLOC_INLINE unsigned
arena_ind_get(const arena_t *arena) {
	return base_ind_get(arena->base);
}

JEMALLOC_INLINE void
arena_internal_add(arena_t *arena, size_t size) {
	atomic_fetch_add_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

JEMALLOC_INLINE void
arena_internal_sub(arena_t *arena, size_t size) {
	atomic_fetch_sub_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

JEMALLOC_INLINE size_t
arena_internal_get(arena_t *arena) {
	return atomic_load_zu(&arena->stats.internal, ATOMIC_RELAXED);
}

JEMALLOC_INLINE bool
arena_prof_accum(tsdn_t *tsdn, arena_t *arena, uint64_t accumbytes) {
	cassert(config_prof);

	if (likely(prof_interval == 0)) {
		return false;
	}

	return prof_accum_add(tsdn, &arena->prof_accum, accumbytes);
}

JEMALLOC_INLINE void
percpu_arena_update(tsd_t *tsd, unsigned cpu) {
	assert(have_percpu_arena);
	arena_t *oldarena = tsd_arena_get(tsd);
	assert(oldarena != NULL);
	unsigned oldind = arena_ind_get(oldarena);

	if (oldind != cpu) {
		unsigned newind = cpu;
		arena_t *newarena = arena_get(tsd_tsdn(tsd), newind, true);
		assert(newarena != NULL);

		/* Set new arena/tcache associations. */
		arena_migrate(tsd, oldind, newind);
		if (config_tcache) {
			tcache_t *tcache = tsd_tcache_get(tsd);
			if (tcache) {
				tcache_arena_reassociate(tsd_tsdn(tsd), tcache,
				    newarena);
			}
		}
	}
}

#endif /* (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ARENA_C_)) */

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_A_H */
