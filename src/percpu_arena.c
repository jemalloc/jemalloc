#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/percpu_arena.h"

#include "jemalloc/internal/assert.h"

/******************************************************************************/
/* Data. */

/*
 * Define names for both uninitialized and initialized phases, so that
 * options and mallctl processing are straightforward.
 */
const char *const percpu_arena_mode_names[] = {
    "percpu", "phycpu", "disabled", "percpu", "phycpu"};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

uint16_t percpu_arena_map[PERCPU_ARENA_MAX_CPUS];
unsigned percpu_arena_ngroups;

/******************************************************************************/

/* How many distinct arenas the mode spreads ncpus_mapped CPUs over. */
static unsigned
percpu_arena_ngroups_for(percpu_arena_mode_t mode, unsigned ncpus_mapped) {
	assert(PERCPU_ARENA_ENABLED(mode));
	assert(ncpus_mapped > 0);

	switch (mode) {
	case percpu_arena:
		return ncpus_mapped;
	case per_phycpu_arena:
		/*
		 * Hyper threads on the same physical CPU share an arena.  An
		 * odd count likely means a misconfig; round up so that the
		 * unpaired CPU still has a group of its own.
		 */
		return ncpus_mapped > 1
		    ? ncpus_mapped / 2 + ncpus_mapped % 2
		    : ncpus_mapped;
	default:
		not_reached();
	}
}

/* Which of those groups the CPU ranked cpu_pos in the mapped set falls in. */
static unsigned
percpu_arena_group_of(percpu_arena_mode_t mode, unsigned cpu_pos,
    unsigned ncpus_mapped) {
	switch (mode) {
	case percpu_arena:
		return cpu_pos;
	case per_phycpu_arena:
		return cpu_pos < ncpus_mapped / 2 ? cpu_pos
		                                  : cpu_pos - ncpus_mapped / 2;
	default:
		not_reached();
	}
}

unsigned
percpu_arena_min_narenas(percpu_arena_mode_t mode) {
	assert(ncpus > 0);
	return percpu_arena_ngroups_for(mode, ncpus);
}

void
percpu_arena_map_build(uint16_t *map, size_t map_len, percpu_arena_mode_t mode,
    const unsigned *cpu_ids, unsigned ncpus_mapped, unsigned *ngroups) {
	assert(map_len > 0);

	unsigned n = percpu_arena_ngroups_for(mode, ncpus_mapped);

	/*
	 * CPU ids we know nothing about keep a bounded fallback.  If the
	 * process later moves to a CPU that was not allowed at boot, the index
	 * stays valid even though it may not preserve the affinity rank
	 * mapping.
	 */
	for (size_t c = 0; c < map_len; c++) {
		map[c] = (uint16_t)(c % n);
	}

	for (unsigned pos = 0; pos < ncpus_mapped; pos++) {
		unsigned cpu = (cpu_ids != NULL) ? cpu_ids[pos] : pos;
		if (cpu >= map_len) {
			continue;
		}
		unsigned ind = percpu_arena_group_of(mode, pos, ncpus_mapped);
		assert(ind < n);
		map[cpu] = (uint16_t)ind;
	}

	*ngroups = n;
}

void
percpu_arena_boot(percpu_arena_mode_t mode, unsigned narenas) {
	assert(ncpus > 0);
	assert(narenas > 0);

	/*
	 * Boot runs once, from malloc_init_hard() under init_lock, so a static
	 * scratch buffer is safe and keeps this off the caller's stack.  It is
	 * only ever touched when percpu arenas are enabled.
	 */
	static unsigned cpu_ids[PERCPU_ARENA_MAX_CPUS];
	unsigned ncpu_ids =
	    os_cpu_affinity_cpus(cpu_ids, PERCPU_ARENA_MAX_CPUS);
	/* Only trust the mask if it accounts for every CPU we counted. */
	const unsigned *map_cpu_ids = (ncpu_ids == ncpus) ? cpu_ids : NULL;

	percpu_arena_map_build(percpu_arena_map, PERCPU_ARENA_MAX_CPUS, mode,
	    map_cpu_ids, ncpus, &percpu_arena_ngroups);

	assert(percpu_arena_ngroups > 0);
	assert(percpu_arena_ngroups <= narenas);
}
