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
    "percpu", "disabled", "percpu"};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

uint16_t percpu_arena_map[PERCPU_ARENA_MAX_CPUS];

/******************************************************************************/

void
percpu_arena_map_build(uint16_t *map, size_t map_len, const unsigned *cpus,
    unsigned ncpus_mapped) {
	assert(map_len > 0);
	assert(ncpus_mapped > 0 && ncpus_mapped < MALLOCX_ARENA_LIMIT);

	/*
	 * CPUs outside the startup mask share its arenas through a bounded
	 * fallback.  This also handles unavailable affinity information.
	 */
	for (size_t c = 0; c < map_len; c++) {
		map[c] = (uint16_t)(c % ncpus_mapped);
	}
	if (cpus == NULL) {
		return;
	}

	/* Give each allowed CPU its own arena, in affinity rank order. */
	for (unsigned pos = 0; pos < ncpus_mapped; pos++) {
		unsigned cpu = cpus[pos];
		if (cpu < map_len) {
			map[cpu] = (uint16_t)pos;
		}
	}
}

void
percpu_arena_boot(void) {
	assert(ncpus > 0 && ncpus <= PERCPU_ARENA_MAX_CPUS);

	/*
	 * Boot runs from malloc_init_hard() under init_lock, so a static scratch
	 * buffer is safe and keeps it off the caller's stack.  It is only ever
	 * touched when percpu arenas are enabled.
	 */
	static unsigned cpus[PERCPU_ARENA_MAX_CPUS];
	/*
	 * Only trust the mask if it accounts for every CPU we counted; otherwise
	 * assume ids 0..ncpus-1.
	 */
	bool trusted = os_cpu_affinity_cpus(cpus, PERCPU_ARENA_MAX_CPUS)
	    == ncpus;

	percpu_arena_map_build(percpu_arena_map, PERCPU_ARENA_MAX_CPUS,
	    trusted ? cpus : NULL, ncpus);
}
