#ifndef JEMALLOC_INTERNAL_PERCPU_ARENA_H
#define JEMALLOC_INTERNAL_PERCPU_ARENA_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/os/cpu.h"
#include "jemalloc/internal/util.h"

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

/*
 * Size of the CPU id -> arena index table.  4096 entries because:
 *   - MALLOCX_ARENA_LIMIT is 4095, so every CPU id that the legacy identity
 *     mapping could have used as an arena index fits;
 *   - os_cpu_current()'s rdtscp fallback masks with 0xfff, so it can never
 *     produce an id outside the table.
 * The whole table is populated at boot; entries past the CPUs we know about
 * wrap modulo the group count, so the read path needs no bounds handling
 * beyond the range check against the table size.
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
/* Number of distinct arena indices the map produces.  <= narenas_auto. */
extern unsigned percpu_arena_ngroups;

/*
 * Fill map[0, map_len) and *ngroups with the mode's CPU -> arena mapping.  If
 * cpu_ids is non-NULL, the ncpus_mapped entries are the actual CPU ids in rank
 * order; otherwise CPUs are assumed to be compactly numbered 0..n-1.  Pure:
 * depends on nothing but its arguments, and is non-static so that the unit
 * test can drive it with synthetic CPU counts.
 */
void percpu_arena_map_build(uint16_t *map, size_t map_len,
    percpu_arena_mode_t mode, const unsigned *cpu_ids, unsigned ncpus_mapped,
    unsigned *ngroups);

/*
 * Smallest narenas the mode can work with.  Consulted while narenas is still
 * being sized, so it must not depend on the map.
 */
unsigned percpu_arena_min_narenas(percpu_arena_mode_t mode);

/*
 * Build the global map and set percpu_arena_ngroups.  Called once, from
 * malloc_init_narenas() with narenas final and the mode in its *initialized*
 * encoding (opt_percpu_arena is still uninit at that point).  Must run before
 * the first percpu_arena_choose() / percpu_arena_ind_limit().
 */
void percpu_arena_boot(percpu_arena_mode_t mode, unsigned narenas);

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
	assert(percpu_arena_ngroups > 0);

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
		return cpu % percpu_arena_ngroups;
	}

	return percpu_arena_map[cpu];
}

/* Return the limit of percpu auto arena range, i.e. arenas[0...ind_limit). */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_ind_limit(percpu_arena_mode_t mode) {
	assert(have_percpu_arena && PERCPU_ARENA_ENABLED(mode));
	assert(percpu_arena_ngroups > 0);
	return percpu_arena_ngroups;
}

#endif /* JEMALLOC_INTERNAL_PERCPU_ARENA_H */
