#ifndef JEMALLOC_INTERNAL_PERCPU_ARENA_H
#define JEMALLOC_INTERNAL_PERCPU_ARENA_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/os/cpu.h"

/******************************************************************************/
/* TYPES */
/******************************************************************************/

typedef enum {
	percpu_arena_mode_names_base = 0, /* Used for options processing. */

	/*
	 * *_uninit are used only during bootstrapping, and must correspond
	 * to initialized variant plus percpu_arena_mode_enabled_base.
	 */
	percpu_arena_uninit = 0,
	per_phycpu_arena_uninit = 1,

	/* All non-disabled modes must come after percpu_arena_disabled. */
	percpu_arena_disabled = 2,

	percpu_arena_mode_names_limit = 3, /* Used for options processing. */
	percpu_arena_mode_enabled_base = 3,

	percpu_arena = 3,
	per_phycpu_arena = 4 /* Hyper threads share arena. */
} percpu_arena_mode_t;

#define PERCPU_ARENA_ENABLED(m) ((m) >= percpu_arena_mode_enabled_base)
#define PERCPU_ARENA_DEFAULT percpu_arena_disabled

/******************************************************************************/
/* EXTERNS */
/******************************************************************************/

extern percpu_arena_mode_t opt_percpu_arena;
extern const char *const   percpu_arena_mode_names[];

/******************************************************************************/
/* INLINES */
/******************************************************************************/

JEMALLOC_ALWAYS_INLINE malloc_cpuid_t
malloc_getcpu(void) {
	assert(have_percpu_arena);
	return (malloc_cpuid_t)os_cpu_current();
}

/* Return the chosen arena index based on current cpu. */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_choose(void) {
	assert(have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena));

	malloc_cpuid_t cpuid = malloc_getcpu();
	assert(cpuid >= 0);

	unsigned arena_ind;
	if ((opt_percpu_arena == percpu_arena)
	    || ((unsigned)cpuid < ncpus / 2)) {
		arena_ind = cpuid;
	} else {
		assert(opt_percpu_arena == per_phycpu_arena);
		/* Hyper threads on the same physical CPU share arena. */
		arena_ind = cpuid - ncpus / 2;
	}

	return arena_ind;
}

/* Return the limit of percpu auto arena range, i.e. arenas[0...ind_limit). */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_ind_limit(percpu_arena_mode_t mode) {
	assert(have_percpu_arena && PERCPU_ARENA_ENABLED(mode));
	if (mode == per_phycpu_arena && ncpus > 1) {
		if (ncpus % 2) {
			/* This likely means a misconfig. */
			return ncpus / 2 + 1;
		}
		return ncpus / 2;
	} else {
		return ncpus;
	}
}

#endif /* JEMALLOC_INTERNAL_PERCPU_ARENA_H */
