#ifndef JEMALLOC_INTERNAL_PERCPU_ARENA_H
#define JEMALLOC_INTERNAL_PERCPU_ARENA_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/os/cpu.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/
/* TYPES */
/******************************************************************************/

typedef enum {
	percpu_arena_mode_names_base = 0, /* Used for options processing. */

	/*
	 * The uninitialized mode is used only during bootstrapping, and must
	 * correspond to the initialized mode minus the enabled base.
	 */
	percpu_arena_uninit = 0,

	/* All non-disabled modes must come after percpu_arena_disabled. */
	percpu_arena_disabled = 1,

	percpu_arena_mode_names_limit = 2, /* Used for options processing. */
	percpu_arena_mode_enabled_base = 2,

	percpu_arena = 2
} percpu_arena_mode_t;

#define PERCPU_ARENA_ENABLED(m) ((m) >= percpu_arena_mode_enabled_base)
#define PERCPU_ARENA_DEFAULT percpu_arena_disabled

/*
 * Size of the CPU id -> arena index table.  os_cpu_current()'s CPU-register
 * fallbacks mask with 0xfff, so 4096 entries cover every id they can produce.
 * Other backends may return a larger id; percpu_arena_choose() handles those
 * without indexing the table.
 * The whole table is populated at boot; entries past the CPUs we know about
 * wrap modulo the startup CPU count, so the read path needs no bounds
 * handling beyond the range check against the table size.
 */
#define PERCPU_ARENA_MAX_CPUS 4096

/******************************************************************************/
/* EXTERNS */
/******************************************************************************/

extern percpu_arena_mode_t opt_percpu_arena;
extern const char *const   percpu_arena_mode_names[];

/*
 * CPU id -> arena index.  Immutable once percpu_arena_boot() returns; the read
 * path is lock-free and never revalidates, so a CPU coming online later keeps
 * whatever entry boot gave it.
 */
extern uint16_t percpu_arena_map[PERCPU_ARENA_MAX_CPUS];

/*
 * Fill map[0, map_len).  The ncpus_mapped distinct CPUs in cpus get arena
 * indices in rank order; other CPUs wrap modulo ncpus_mapped.  If cpus is
 * NULL, assume dense ids 0..ncpus_mapped-1.  CPU ids beyond map_len use the
 * read path's modulo fallback.  Pure and non-static for synthetic tests.
 */
void percpu_arena_map_build(uint16_t *map, size_t map_len,
    const unsigned *cpus, unsigned ncpus_mapped);

/*
 * Build the global map for ncpus automatic arenas.  Called from
 * malloc_init_narenas() under init_lock before narenas is sized;
 * opt_percpu_arena is still uninit, so nothing reads the map yet.
 */
void percpu_arena_boot(void);

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
	assert(ncpus > 0);

	malloc_cpuid_t cpuid = malloc_getcpu();
	assert(cpuid >= 0);

	unsigned cpu = (unsigned)cpuid;
	if (unlikely(cpu >= PERCPU_ARENA_MAX_CPUS)) {
		/*
		 * Reachable only where sched_getcpu() reports an id the table
		 * cannot hold, i.e. a kernel built for more than
		 * PERCPU_ARENA_MAX_CPUS processors.  Keep the result in range
		 * rather than indexing off the end of the table.
		 */
		return cpu % ncpus;
	}

	return percpu_arena_map[cpu];
}

/* Return the limit of percpu auto arena range, i.e. arenas[0...ind_limit). */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_ind_limit(void) {
	assert(have_percpu_arena
	    && PERCPU_ARENA_ENABLED(opt_percpu_arena));
	assert(ncpus > 0);
	return ncpus;
}

#endif /* JEMALLOC_INTERNAL_PERCPU_ARENA_H */
