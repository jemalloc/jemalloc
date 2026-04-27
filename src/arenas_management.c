#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/sz.h"

JEMALLOC_ALIGNED(CACHELINE)
atomic_p_t arenas[MALLOCX_ARENA_LIMIT];
/* Below two are read-only after initialization. */
unsigned   narenas_auto;
unsigned   manual_arena_base;

static atomic_u_t narenas_total;

static malloc_mutex_t arenas_lock;

bool
arenas_management_boot(void) {
	return malloc_mutex_init(&arenas_lock, "arenas", WITNESS_RANK_ARENAS,
	    malloc_mutex_rank_exclusive);
}

void
arenas_management_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &arenas_lock);
}

void
arenas_management_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &arenas_lock);
}

void
arenas_management_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &arenas_lock);
}

void
narenas_total_set(unsigned narenas) {
	atomic_store_u(&narenas_total, narenas, ATOMIC_RELEASE);
}

void
narenas_total_inc(void) {
	atomic_fetch_add_u(&narenas_total, 1, ATOMIC_RELEASE);
}

unsigned
narenas_total_get(void) {
	return atomic_load_u(&narenas_total, ATOMIC_ACQUIRE);
}

void
narenas_auto_set(unsigned n) {
	narenas_auto = n;
}

void
manual_arena_base_set(unsigned base) {
	manual_arena_base = base;
}

/*
 * The a0*() functions are used instead of i{d,}alloc() in situations that
 * cannot tolerate TLS variable access.
 */

void *
a0ialloc(size_t size, bool zero, bool is_internal) {
	if (unlikely(malloc_init_state == malloc_init_uninitialized)
	    && malloc_init_hard_a0()) {
		return NULL;
	}

	return iallocztm(TSDN_NULL, size, sz_size2index(size), zero, NULL,
	    is_internal, arena_get(TSDN_NULL, 0, true), true);
}

void
a0idalloc(void *ptr, bool is_internal) {
	idalloctm(TSDN_NULL, ptr, NULL, NULL, is_internal, true);
}

void *
a0malloc(size_t size) {
	return a0ialloc(size, false, true);
}

void
a0dalloc(void *ptr) {
	a0idalloc(ptr, true);
}

void
arena_set(unsigned ind, arena_t *arena) {
	atomic_store_p(&arenas[ind], arena, ATOMIC_RELEASE);
}

/* Create a new arena and insert it into the arenas array at index ind. */
static arena_t *
arena_init_locked(tsdn_t *tsdn, unsigned ind, const arena_config_t *config) {
	arena_t *arena;

	assert(ind <= narenas_total_get());
	if (ind >= MALLOCX_ARENA_LIMIT) {
		return NULL;
	}
	if (ind == narenas_total_get()) {
		narenas_total_inc();
	}

	/*
	 * Another thread may have already initialized arenas[ind] if it's an
	 * auto arena.
	 */
	arena = arena_get(tsdn, ind, false);
	if (arena != NULL) {
		assert(arena_is_auto(arena));
		return arena;
	}

	/* Actually initialize the arena. */
	arena = arena_new(tsdn, ind, config);

	return arena;
}

static void
arena_new_create_background_thread(tsdn_t *tsdn, unsigned ind) {
	if (ind == 0) {
		return;
	}

	if (have_background_thread) {
		if (background_thread_create(tsdn_tsd(tsdn), ind)) {
			malloc_printf(
			    "<jemalloc>: error in background thread "
			    "creation for arena %u. Abort.\n",
			    ind);
			abort();
		}
	}
}

arena_t *
arena_init(tsdn_t *tsdn, unsigned ind, const arena_config_t *config) {
	arena_t *arena;

	malloc_mutex_lock(tsdn, &arenas_lock);
	arena = arena_init_locked(tsdn, ind, config);
	malloc_mutex_unlock(tsdn, &arenas_lock);

	arena_new_create_background_thread(tsdn, ind);

	return arena;
}

static void
arena_bind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_inc(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, arena);
	} else {
		tsd_arena_set(tsd, arena);
		/*
		 * While shard acts as a random seed, the cast below should
		 * not make much difference.
		 */
		uint8_t shard = (uint8_t)atomic_fetch_add_u(
		    &arena->binshard_next, 1, ATOMIC_RELAXED);
		tsd_binshards_t *bins = tsd_binshardsp_get(tsd);
		for (unsigned i = 0; i < SC_NBINS; i++) {
			assert(bin_infos[i].n_shards > 0
			    && bin_infos[i].n_shards <= BIN_SHARDS_MAX);
			bins->binshard[i] = shard % bin_infos[i].n_shards;
		}
	}
}

void
arena_migrate(tsd_t *tsd, arena_t *oldarena, arena_t *newarena) {
	assert(oldarena != NULL);
	assert(newarena != NULL);

	arena_nthreads_dec(oldarena, false);
	arena_nthreads_inc(newarena, false);
	tsd_arena_set(tsd, newarena);

	if (arena_nthreads_get(oldarena, false) == 0
	    && !background_thread_enabled()) {
		/*
		 * Purge if the old arena has no associated threads anymore and
		 * no background threads.
		 */
		arena_decay(tsd_tsdn(tsd), oldarena,
		    /* is_background_thread */ false, /* all */ true);
	}
}

static void
arena_unbind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena;

	arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_dec(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, NULL);
	} else {
		tsd_arena_set(tsd, NULL);
	}
}

/* Slow path, called only by arena_choose(). */
arena_t *
arena_choose_hard(tsd_t *tsd, bool internal) {
	arena_t *ret JEMALLOC_CC_SILENCE_INIT(NULL);

	if (have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena)) {
		unsigned choose = percpu_arena_choose();
		ret = arena_get(tsd_tsdn(tsd), choose, true);
		assert(ret != NULL);
		arena_bind(tsd, arena_ind_get(ret), false);
		arena_bind(tsd, arena_ind_get(ret), true);

		return ret;
	}

	if (narenas_auto > 1) {
		unsigned i, j, choose[2], first_null;
		bool     is_new_arena[2];

		/*
		 * Determine binding for both non-internal and internal
		 * allocation.
		 *
		 *   choose[0]: For application allocation.
		 *   choose[1]: For internal metadata allocation.
		 */

		for (j = 0; j < 2; j++) {
			choose[j] = 0;
			is_new_arena[j] = false;
		}

		first_null = narenas_auto;
		malloc_mutex_lock(tsd_tsdn(tsd), &arenas_lock);
		assert(arena_get(tsd_tsdn(tsd), 0, false) != NULL);
		for (i = 1; i < narenas_auto; i++) {
			if (arena_get(tsd_tsdn(tsd), i, false) != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				for (j = 0; j < 2; j++) {
					if (arena_nthreads_get(
					        arena_get(
					            tsd_tsdn(tsd), i, false),
					        !!j)
					    < arena_nthreads_get(
					        arena_get(tsd_tsdn(tsd),
					            choose[j], false),
					        !!j)) {
						choose[j] = i;
					}
				}
			} else if (first_null == narenas_auto) {
				/*
				 * Record the index of the first uninitialized
				 * arena, in case all extant arenas are in use.
				 *
				 * NB: It is possible for there to be
				 * discontinuities in terms of initialized
				 * versus uninitialized arenas, due to the
				 * "thread.arena" mallctl.
				 */
				first_null = i;
			}
		}

		for (j = 0; j < 2; j++) {
			if (arena_nthreads_get(
			        arena_get(tsd_tsdn(tsd), choose[j], false), !!j)
			        == 0
			    || first_null == narenas_auto) {
				/*
				 * Use an unloaded arena, or the least loaded
				 * arena if all arenas are already initialized.
				 */
				if (!!j == internal) {
					ret = arena_get(
					    tsd_tsdn(tsd), choose[j], false);
				}
			} else {
				arena_t *arena;

				/* Initialize a new arena. */
				choose[j] = first_null;
				arena = arena_init_locked(tsd_tsdn(tsd),
				    choose[j], &arena_config_default);
				if (arena == NULL) {
					malloc_mutex_unlock(
					    tsd_tsdn(tsd), &arenas_lock);
					return NULL;
				}
				is_new_arena[j] = true;
				if (!!j == internal) {
					ret = arena;
				}
			}
			arena_bind(tsd, choose[j], !!j);
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &arenas_lock);

		for (j = 0; j < 2; j++) {
			if (is_new_arena[j]) {
				assert(choose[j] > 0);
				arena_new_create_background_thread(
				    tsd_tsdn(tsd), choose[j]);
			}
		}

	} else {
		ret = arena_get(tsd_tsdn(tsd), 0, false);
		arena_bind(tsd, 0, false);
		arena_bind(tsd, 0, true);
	}

	return ret;
}

void
iarena_cleanup(tsd_t *tsd) {
	arena_t *iarena;

	iarena = tsd_iarena_get(tsd);
	if (iarena != NULL) {
		arena_unbind(tsd, arena_ind_get(iarena), true);
	}
}

void
arena_cleanup(tsd_t *tsd) {
	arena_t *arena;

	arena = tsd_arena_get(tsd);
	if (arena != NULL) {
		arena_unbind(tsd, arena_ind_get(arena), false);
	}
}
