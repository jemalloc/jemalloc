#include "test/jemalloc_test.h"

static const unsigned test_ncpus[]
    = {1, 2, 3, 4, 5, 7, 8, 64, 88, 176, 1023, 4094};
#define NNCPUS (sizeof(test_ncpus) / sizeof(test_ncpus[0]))

static uint16_t map[PERCPU_ARENA_MAX_CPUS];
static unsigned cpus[PERCPU_ARENA_MAX_CPUS];
static unsigned keys[PERCPU_ARENA_MAX_CPUS];

/*
 * Invariants every mapping owes its callers: every CPU id maps somewhere in
 * range, and every group is reachable from an allowed CPU.
 */
static void
expect_map_well_formed(unsigned n, unsigned ngroups) {
	static bool seen[PERCPU_ARENA_MAX_CPUS];

	expect_u_gt(ngroups, 0, "Group count must be positive (ncpus %u)", n);
	expect_u_le(ngroups, n,
	    "Group count must not exceed the CPU count (ncpus %u)", n);

	/*
	 * An affinity-restricted process counts only the CPUs in its mask but
	 * is still told the machine-wide CPU id, so ids outside the mask are
	 * reachable and must still land on a real arena.
	 */
	for (unsigned cpu = 0; cpu < PERCPU_ARENA_MAX_CPUS; cpu++) {
		expect_u_lt(map[cpu], ngroups,
		    "Every CPU id must map into the group range (ncpus %u, "
		    "cpu %u)",
		    n, cpu);
	}

	memset(seen, 0, sizeof(seen));
	for (unsigned pos = 0; pos < n; pos++) {
		seen[map[cpus[pos]]] = true;
	}
	for (unsigned g = 0; g < ngroups; g++) {
		expect_true(seen[g], "Arena %u is unreachable (ncpus %u)", g, n);
	}
}

static unsigned
build(unsigned n) {
	unsigned ngroups;
	percpu_arena_map_build(
	    map, PERCPU_ARENA_MAX_CPUS, cpus, keys, n, &ngroups);
	expect_map_well_formed(n, ngroups);
	return ngroups;
}

TEST_BEGIN(test_percpu_dense) {
	for (unsigned i = 0; i < NNCPUS; i++) {
		unsigned n = test_ncpus[i];
		for (unsigned cpu = 0; cpu < n; cpu++) {
			cpus[cpu] = keys[cpu] = cpu;
		}
		expect_u_eq(build(n), n,
		    "percpu should use one arena per CPU (ncpus %u)", n);
		for (unsigned cpu = 0; cpu < n; cpu++) {
			expect_u_eq(map[cpu], cpu,
			    "CPU %u should use arena %u (ncpus %u)", cpu, cpu, n);
		}
	}
}
TEST_END

TEST_BEGIN(test_percpu_sparse_uses_affinity_rank) {
	unsigned sparse[] = {150, 154, 158, 162};
	for (unsigned pos = 0; pos < 4; pos++) {
		cpus[pos] = keys[pos] = sparse[pos];
	}
	expect_u_eq(build(4), 4, "percpu should use one arena per allowed CPU");
	for (unsigned pos = 0; pos < 4; pos++) {
		expect_u_eq(map[sparse[pos]], pos,
		    "Allowed CPU %u should use arena %u", sparse[pos], pos);
	}
}
TEST_END

/* Allowed CPUs first..first+n-1, keyed as core_key() says. */
static unsigned
build_range(
    unsigned first, unsigned n, unsigned (*core_key)(unsigned cpu)) {
	for (unsigned pos = 0; pos < n; pos++) {
		cpus[pos] = first + pos;
		keys[pos] = core_key(first + pos);
	}
	return build(n);
}

static unsigned
key_half_split(unsigned cpu) {
	/* 176 CPUs, siblings N and N + 88 (x86 Linux numbering). */
	return cpu % 88;
}

static unsigned
key_adjacent(unsigned cpu) {
	/* Siblings 2k and 2k + 1 (Windows, FreeBSD, many VMs). */
	return cpu & ~1U;
}

static unsigned
key_smt4(unsigned cpu) {
	return cpu & ~3U;
}

static unsigned
key_no_smt(unsigned cpu) {
	return cpu;
}

TEST_BEGIN(test_phycpu_groups_by_core) {
	expect_u_eq(build_range(0, 176, key_half_split), 88,
	    "Full mask with N / N + 88 siblings should give 88 arenas");
	for (unsigned cpu = 0; cpu < 88; cpu++) {
		expect_u_eq(map[cpu], map[cpu + 88],
		    "CPU %u and its sibling %u should share an arena", cpu,
		    cpu + 88);
		expect_u_eq(map[cpu], cpu, "Core %u should use arena %u", cpu,
		    cpu);
	}

	/* The case rank pairing got wrong: 16 distinct cores. */
	expect_u_eq(build_range(0, 16, key_half_split), 16,
	    "CPUs 0-15 are 16 cores and should get 16 arenas");

	expect_u_eq(build_range(0, 8, key_adjacent), 4,
	    "Adjacent siblings should pair up");
	expect_u_eq(map[0], map[1], "CPUs 0 and 1 should share an arena");
	expect_u_ne(map[1], map[2], "CPUs 1 and 2 should not share an arena");

	expect_u_eq(build_range(0, 16, key_smt4), 4,
	    "Four-way SMT should put four CPUs on each arena");
	expect_u_eq(map[4], map[7], "CPUs 4-7 should share an arena");

	expect_u_eq(build_range(0, 7, key_no_smt), 7,
	    "Without SMT every CPU should get its own arena");
}
TEST_END

TEST_BEGIN(test_phycpu_partial_and_unknown_cores) {
	/* Allowed 5, 88, 93 with siblings N / N + 88: core 0 is partial. */
	unsigned partial_cpus[] = {5, 88, 93};
	unsigned partial_keys[] = {5, 0, 5};
	for (unsigned pos = 0; pos < 3; pos++) {
		cpus[pos] = partial_cpus[pos];
		keys[pos] = partial_keys[pos];
	}
	expect_u_eq(build(3), 2, "Two cores should give two arenas");
	expect_u_eq(map[5], map[93], "CPUs 5 and 93 should share an arena");
	expect_u_ne(map[5], map[88], "CPUs 5 and 88 should not share an arena");
	expect_u_eq(map[0], map[88],
	    "A disallowed sibling should map to its core's arena");

	/* Unknown cores keep their own ids and are never merged. */
	unsigned unknown[] = {3, 91};
	for (unsigned pos = 0; pos < 2; pos++) {
		cpus[pos] = keys[pos] = unknown[pos];
	}
	expect_u_eq(build(2), 2, "CPUs of unknown cores must not share");

	/* A key the map cannot index falls back to the CPU's own id. */
	cpus[0] = 1;
	keys[0] = PERCPU_ARENA_MAX_CPUS;
	cpus[1] = 2;
	keys[1] = PERCPU_ARENA_MAX_CPUS;
	expect_u_eq(build(2), 2, "Out-of-range keys must not merge CPUs");
}
TEST_END

TEST_BEGIN(test_core_key_host) {
	unsigned n = os_cpu_affinity_cpus(cpus, PERCPU_ARENA_MAX_CPUS);
	unsigned key;
	test_skip_if(n == 0 || os_cpu_core_key(cpus[0], &key));

	for (unsigned pos = 0; pos < n; pos++) {
		unsigned cpu = cpus[pos];
		if (os_cpu_core_key(cpu, &key)) {
			continue;
		}
		expect_u_le(key, cpu,
		    "CPU %u's core key must be its smallest sibling", cpu);
		unsigned key_of_key;
		if (!os_cpu_core_key(key, &key_of_key)) {
			expect_u_eq(key_of_key, key,
			    "CPU %u's core key %u must key to itself", cpu, key);
		}
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_percpu_dense,
	    test_percpu_sparse_uses_affinity_rank, test_phycpu_groups_by_core,
	    test_phycpu_partial_and_unknown_cores, test_core_key_host);
}
