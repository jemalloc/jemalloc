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

#define PERCPU_ARENA_UNMAPPED UINT16_MAX

void
percpu_arena_map_build(uint16_t *map, size_t map_len, const unsigned *cpus,
    const unsigned *keys, unsigned ncpus_mapped, unsigned *ngroups) {
	assert(map_len > 0);
	assert(ncpus_mapped > 0);

	/*
	 * map[key] doubles as the key -> group table.  That is consistent: a key
	 * is a CPU on the same core, so it belongs to the same group, and it
	 * also sends a disallowed sibling of an allowed CPU to its core's arena.
	 */
	for (size_t c = 0; c < map_len; c++) {
		map[c] = PERCPU_ARENA_UNMAPPED;
	}
	unsigned n = 0;
	for (unsigned pos = 0; pos < ncpus_mapped; pos++) {
		unsigned cpu = cpus[pos];
		if (cpu >= map_len) {
			continue;
		}
		unsigned key = keys[pos] < map_len ? keys[pos] : cpu;
		if (map[key] == PERCPU_ARENA_UNMAPPED) {
			assert(n < MALLOCX_ARENA_LIMIT);
			map[key] = (uint16_t)n++;
		}
		map[cpu] = map[key];
	}
	assert(n > 0);

	/*
	 * CPU ids we know nothing about keep a bounded fallback.  If the
	 * process later moves to a CPU that was not allowed at boot, the index
	 * stays valid even though it may not preserve the affinity rank
	 * mapping.
	 */
	for (size_t c = 0; c < map_len; c++) {
		if (map[c] == PERCPU_ARENA_UNMAPPED) {
			map[c] = (uint16_t)(c % n);
		}
	}

	*ngroups = n;
}

unsigned
percpu_arena_boot(percpu_arena_mode_t mode) {
	assert(ncpus > 0 && ncpus <= PERCPU_ARENA_MAX_CPUS);
	assert(PERCPU_ARENA_ENABLED(mode));

	/*
	 * Boot runs from malloc_init_hard() under init_lock, so static scratch
	 * buffers are safe and keep these off the caller's stack.  They are
	 * only ever touched when percpu arenas are enabled.
	 */
	static unsigned cpus[PERCPU_ARENA_MAX_CPUS];
	static unsigned keys[PERCPU_ARENA_MAX_CPUS];
	/*
	 * Only trust the mask if it accounts for every CPU we counted; otherwise
	 * assume ids 0..ncpus-1, which say nothing about cores.
	 */
	bool trusted = os_cpu_affinity_cpus(cpus, PERCPU_ARENA_MAX_CPUS)
	    == ncpus;
	for (unsigned pos = 0; pos < ncpus; pos++) {
		if (!trusted) {
			cpus[pos] = pos;
		}
		keys[pos] = cpus[pos];
		/*
		 * A CPU whose core is unknown keeps its own id as key, and so its
		 * own arena: never merge CPUs on a guess.
		 */
		if (mode == per_phycpu_arena && trusted) {
			os_cpu_core_key(cpus[pos], &keys[pos]);
		}
	}

	percpu_arena_map_build(percpu_arena_map, PERCPU_ARENA_MAX_CPUS, cpus,
	    keys, ncpus, &percpu_arena_ngroups);
	return percpu_arena_ngroups;
}
