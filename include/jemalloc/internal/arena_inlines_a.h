#ifndef JEMALLOC_INTERNAL_ARENA_INLINES_A_H
#define JEMALLOC_INTERNAL_ARENA_INLINES_A_H

static inline void
arena_slab_data_init(extent_t *slab, arena_slab_data_t *slab_data,
    const bitmap_info_t *binfo, bool fill) {
	if (binfo->nbits <= 8) {
		bitmap_init(&slab_data->internal.mesh_data.bitmap,
		    binfo, fill);
		ql_elm_new(slab, e_slab_data.internal.mesh_data.ql_link);
	} else {
		bitmap_init(slab_data->internal.full_bitmap,
		    binfo, fill);
	}
}

static inline bitmap_t *
arena_slab_data_bitmap_get(arena_slab_data_t *slab_data, const bitmap_info_t *binfo) {
	if (binfo->nbits <= 8) {
		return &slab_data->internal.mesh_data.bitmap;
	} else {
		return slab_data->internal.full_bitmap;
	}
}

static inline unsigned
arena_ind_get(const arena_t *arena) {
	return base_ind_get(arena->base);
}

static inline void
arena_internal_add(arena_t *arena, size_t size) {
	atomic_fetch_add_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

static inline void
arena_internal_sub(arena_t *arena, size_t size) {
	atomic_fetch_sub_zu(&arena->stats.internal, size, ATOMIC_RELAXED);
}

static inline size_t
arena_internal_get(arena_t *arena) {
	return atomic_load_zu(&arena->stats.internal, ATOMIC_RELAXED);
}

static inline bool
arena_prof_accum(tsdn_t *tsdn, arena_t *arena, uint64_t accumbytes) {
	cassert(config_prof);

	if (likely(prof_interval == 0 || !prof_active_get_unlocked())) {
		return false;
	}

	return prof_accum_add(tsdn, &arena->prof_accum, accumbytes);
}

static inline void
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
		tcache_t *tcache = tcache_get(tsd);
		if (tcache != NULL) {
			tcache_arena_reassociate(tsd_tsdn(tsd), tcache,
			    newarena);
		}
	}
}

#endif /* JEMALLOC_INTERNAL_ARENA_INLINES_A_H */
