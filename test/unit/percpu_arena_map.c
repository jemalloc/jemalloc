#include "test/jemalloc_test.h"

/*
 * Expected results for dense CPU IDs, kept separate from the map builder so
 * that the table is checked against an independent implementation.
 */
static unsigned
dense_cpu_ind_limit(percpu_arena_mode_t mode, unsigned ncpus_) {
	if (mode == per_phycpu_arena && ncpus_ > 1) {
		if (ncpus_ % 2) {
			/* This likely means a misconfig. */
			return ncpus_ / 2 + 1;
		}
		return ncpus_ / 2;
	} else {
		return ncpus_;
	}
}

static unsigned
dense_cpu_arena_ind(
    percpu_arena_mode_t mode, unsigned ncpus_, unsigned cpuid) {
	if (mode == percpu_arena || cpuid < ncpus_ / 2) {
		return cpuid;
	} else {
		/* Hyper threads on the same physical CPU share arena. */
		return cpuid - ncpus_ / 2;
	}
}

static const unsigned test_ncpus[]
    = {1, 2, 3, 4, 5, 7, 8, 64, 88, 176, 1023, 4094};
#define NNCPUS (sizeof(test_ncpus) / sizeof(test_ncpus[0]))

static uint16_t map[PERCPU_ARENA_MAX_CPUS];

/*
 * Invariants every mode owes its callers: every CPU id maps somewhere in range,
 * and no arena below the group count is left unreachable.
 */
static void
expect_map_well_formed(percpu_arena_mode_t mode, unsigned ncpus_,
    unsigned ngroups) {
	static bool seen[PERCPU_ARENA_MAX_CPUS];
	const char *name = percpu_arena_mode_names[mode];

	expect_u_gt(ngroups, 0, "Group count must be positive (%s, ncpus %u)",
	    name, ncpus_);
	expect_u_le(ngroups, ncpus_,
	    "Group count must not exceed the CPU count (%s, ncpus %u)", name,
	    ncpus_);

	/*
	 * An affinity-restricted process counts only the CPUs in its mask but
	 * is still told the machine-wide CPU id, so ids at or above the count
	 * are reachable and must still land on a real arena.
	 */
	for (unsigned cpu = 0; cpu < PERCPU_ARENA_MAX_CPUS; cpu++) {
		expect_u_lt(map[cpu], ngroups,
		    "Every CPU id must map into the group range (%s, ncpus %u, "
		    "cpu %u)",
		    name, ncpus_, cpu);
	}

	memset(seen, 0, sizeof(seen));
	for (unsigned cpu = 0; cpu < ncpus_; cpu++) {
		seen[map[cpu]] = true;
	}
	for (unsigned g = 0; g < ngroups; g++) {
		expect_true(seen[g], "Arena %u is unreachable (%s, ncpus %u)", g,
		    name, ncpus_);
	}
}

TEST_BEGIN(test_dense_cpu_mappings) {
	percpu_arena_mode_t modes[] = {percpu_arena, per_phycpu_arena};

	for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		percpu_arena_mode_t mode = modes[m];
		for (unsigned i = 0; i < NNCPUS; i++) {
			unsigned n = test_ncpus[i];
			unsigned ngroups;

			percpu_arena_map_build(
			    map, PERCPU_ARENA_MAX_CPUS, mode, NULL, n,
			    &ngroups);

			expect_u_eq(ngroups, dense_cpu_ind_limit(mode, n),
			    "Group count differs from the dense CPU limit "
			    "(mode %s, ncpus %u)",
			    percpu_arena_mode_names[mode], n);

			for (unsigned cpu = 0; cpu < n; cpu++) {
				expect_u_eq(map[cpu],
				    dense_cpu_arena_ind(mode, n, cpu),
				    "Map differs from the dense CPU mapping "
				    "(mode %s, ncpus %u, cpu %u)",
				    percpu_arena_mode_names[mode], n, cpu);
			}

			expect_map_well_formed(mode, n, ngroups);
		}
	}
}
TEST_END

TEST_BEGIN(test_min_narenas_by_mode) {
	unsigned saved_ncpus = ncpus;

	ncpus = 176;
	expect_u_eq(percpu_arena_min_narenas(percpu_arena), 176,
	    "percpu should require one arena per CPU");
	expect_u_eq(percpu_arena_min_narenas(per_phycpu_arena), 88,
	    "phycpu should require one arena per physical CPU");

	ncpus = saved_ncpus;
}
TEST_END

TEST_BEGIN(test_sparse_cpu_ids_use_affinity_rank) {
	unsigned cpu_ids[] = {150, 154, 158, 162};
	unsigned ngroups;

	percpu_arena_map_build(map, PERCPU_ARENA_MAX_CPUS, percpu_arena,
	    cpu_ids, 4, &ngroups);
	expect_u_eq(ngroups, 4, "percpu should use one arena per allowed CPU");
	expect_u_eq(map[150], 0, "First allowed CPU should use arena 0");
	expect_u_eq(map[154], 1, "Second allowed CPU should use arena 1");
	expect_u_eq(map[158], 2, "Third allowed CPU should use arena 2");
	expect_u_eq(map[162], 3, "Fourth allowed CPU should use arena 3");

	percpu_arena_map_build(map, PERCPU_ARENA_MAX_CPUS, per_phycpu_arena,
	    cpu_ids, 4, &ngroups);
	expect_u_eq(ngroups, 2, "phycpu should use half as many arenas");
	expect_u_eq(map[150], 0, "First allowed CPU should use arena 0");
	expect_u_eq(map[154], 1, "Second allowed CPU should use arena 1");
	expect_u_eq(map[158], 0, "Third allowed CPU should share arena 0");
	expect_u_eq(map[162], 1, "Fourth allowed CPU should share arena 1");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_min_narenas_by_mode,
	    test_dense_cpu_mappings,
	    test_sparse_cpu_ids_use_affinity_rank);
}
