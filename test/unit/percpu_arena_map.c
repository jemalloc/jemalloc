#include "test/jemalloc_test.h"

static const unsigned test_ncpus[]
    = {1, 2, 3, 4, 5, 7, 8, 64, 88, 176, 1023, 4094};
#define NNCPUS (sizeof(test_ncpus) / sizeof(test_ncpus[0]))

static uint16_t map[PERCPU_ARENA_MAX_CPUS];
static unsigned cpus[PERCPU_ARENA_MAX_CPUS];

/*
 * Invariants every mapping owes its callers: every CPU id maps somewhere in
 * range, and each representable allowed CPU gets its own arena.
 */
static void
build(const unsigned *cpu_ids, unsigned n) {
	percpu_arena_map_build(map, PERCPU_ARENA_MAX_CPUS, cpu_ids, n);
	/*
	 * An affinity-restricted process counts only the CPUs in its mask but
	 * is still told the machine-wide CPU id, so ids outside the mask are
	 * reachable and must still land on a real arena.
	 */
	for (unsigned cpu = 0; cpu < PERCPU_ARENA_MAX_CPUS; cpu++) {
		expect_u_lt(map[cpu], n,
		    "Every CPU id must map into the arena range (ncpus %u, "
		    "cpu %u)",
		    n, cpu);
	}

	for (unsigned pos = 0; pos < n; pos++) {
		unsigned cpu = cpu_ids == NULL ? pos : cpu_ids[pos];
		if (cpu < PERCPU_ARENA_MAX_CPUS) {
			expect_u_eq(map[cpu], pos,
			    "Allowed CPU %u should use arena %u", cpu, pos);
		}
	}
}

TEST_BEGIN(test_percpu_dense) {
	for (unsigned i = 0; i < NNCPUS; i++) {
		unsigned n = test_ncpus[i];
		for (unsigned cpu = 0; cpu < n; cpu++) {
			cpus[cpu] = cpu;
		}
		build(cpus, n);
		build(NULL, n);
	}
}
TEST_END

TEST_BEGIN(test_percpu_sparse_uses_affinity_rank) {
	unsigned sparse[] = {150, 154, 158, 162};
	build(sparse, ARRAY_SIZE(sparse));

	unsigned boundary[] = {0, PERCPU_ARENA_MAX_CPUS - 1};
	build(boundary, ARRAY_SIZE(boundary));
}
TEST_END

TEST_BEGIN(test_percpu_unrepresentable_ids) {
	unsigned ids[] = {1, PERCPU_ARENA_MAX_CPUS, UINT_MAX};
	build(ids, ARRAY_SIZE(ids));
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_percpu_dense,
	    test_percpu_sparse_uses_affinity_rank, test_percpu_unrepresentable_ids);
}
