#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa_shard_set.h"

/*
 * Checks what ctl reports for the global HPA shard sets, with the HPA enabled
 * below on supported platforms so that the arenas' own HPA shards hold memory:
 * every stats.hpa_shard_sets leaf equals the sets' merged statistics, and the
 * merged arena is the sum of the arenas plus the sets.  The checks compare
 * ctl with the statistics it is built from, so they hold whatever the HPA's
 * policy; background threads are off so that nothing changes between reads.
 */

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/*
 * An HPA leaf, named as below both "stats.arenas.<i>" and
 * "stats.hpa_shard_sets": "<name>" if group is NULL, else
 * "<group>.<j>.<name>" for j < nindices.
 */
typedef struct hpa_leaf_s {
	const char *group;
	unsigned    nindices;
	const char *name;
	bool        u64;
	uint64_t (*get)(const hpa_shard_stats_t *s, unsigned j);
} hpa_leaf_t;

#define LEAF_GET(id, expr)                                                     \
	static uint64_t leaf_get_##id(                                         \
	    const hpa_shard_stats_t *s, unsigned j) {                          \
		(void)j;                                                       \
		return (uint64_t)(expr);                                       \
	}

LEAF_GET(sec_bytes, s->secstats.bytes)
LEAF_GET(sec_hits, s->secstats.total.nhits)
LEAF_GET(sec_misses, s->secstats.total.nmisses)
LEAF_GET(sec_dalloc_noflush, s->secstats.total.ndalloc_noflush)
LEAF_GET(sec_dalloc_flush, s->secstats.total.ndalloc_flush)
LEAF_GET(sec_overfills, s->secstats.total.noverfills)
LEAF_GET(npageslabs, s->psset_stats.merged.npageslabs)
LEAF_GET(nactive, s->psset_stats.merged.nactive)
LEAF_GET(ndirty, s->psset_stats.merged.ndirty)
LEAF_GET(npurge_passes, s->nonderived_stats.npurge_passes)
LEAF_GET(npurges, s->nonderived_stats.npurges)
LEAF_GET(nhugifies, s->nonderived_stats.nhugifies)
LEAF_GET(nhugify_failures, s->nonderived_stats.nhugify_failures)
LEAF_GET(ndehugifies, s->nonderived_stats.ndehugifies)

/* The per-hugeness leaves of slabs, full_slabs and empty_slabs. */
#define LEAF_GET_SLAB(kind)                                                    \
	LEAF_GET(kind##_npageslabs_nonhuge, s->psset_stats.kind[0].npageslabs) \
	LEAF_GET(kind##_npageslabs_huge, s->psset_stats.kind[1].npageslabs)    \
	LEAF_GET(kind##_nactive_nonhuge, s->psset_stats.kind[0].nactive)       \
	LEAF_GET(kind##_nactive_huge, s->psset_stats.kind[1].nactive)          \
	LEAF_GET(kind##_ndirty_nonhuge, s->psset_stats.kind[0].ndirty)         \
	LEAF_GET(kind##_ndirty_huge, s->psset_stats.kind[1].ndirty)
LEAF_GET_SLAB(slabs)
LEAF_GET_SLAB(full_slabs)
LEAF_GET_SLAB(empty_slabs)
LEAF_GET(nonfull_npageslabs_nonhuge,
    s->psset_stats.nonfull_slabs[j][0].npageslabs)
LEAF_GET(nonfull_npageslabs_huge, s->psset_stats.nonfull_slabs[j][1].npageslabs)
LEAF_GET(nonfull_nactive_nonhuge, s->psset_stats.nonfull_slabs[j][0].nactive)
LEAF_GET(nonfull_nactive_huge, s->psset_stats.nonfull_slabs[j][1].nactive)
LEAF_GET(nonfull_ndirty_nonhuge, s->psset_stats.nonfull_slabs[j][0].ndirty)
LEAF_GET(nonfull_ndirty_huge, s->psset_stats.nonfull_slabs[j][1].ndirty)
LEAF_GET(alloc_min_extents, s->nonderived_stats.hpa_alloc_min_extents[j])
LEAF_GET(alloc_max_extents, s->nonderived_stats.hpa_alloc_max_extents[j])
LEAF_GET(alloc_extents, s->nonderived_stats.hpa_alloc_extents[j])
LEAF_GET(alloc_ps, s->nonderived_stats.hpa_alloc_ps[j])
LEAF_GET(alloc_pages_per_ps, s->nonderived_stats.hpa_alloc_pages_per_ps[j])
LEAF_GET(alloc_extents_per_ps, s->nonderived_stats.hpa_alloc_extents_per_ps[j])

#define LEAF(name, u64, id) {NULL, 0, name, u64, leaf_get_##id}
#define LEAF_SLAB(kind)                                                        \
	LEAF("hpa_shard." #kind ".npageslabs_nonhuge", false,                  \
	    kind##_npageslabs_nonhuge),                                        \
	    LEAF("hpa_shard." #kind ".npageslabs_huge", false,                 \
	        kind##_npageslabs_huge),                                       \
	    LEAF("hpa_shard." #kind ".nactive_nonhuge", false,                 \
	        kind##_nactive_nonhuge),                                       \
	    LEAF("hpa_shard." #kind ".nactive_huge", false,                    \
	        kind##_nactive_huge),                                          \
	    LEAF("hpa_shard." #kind ".ndirty_nonhuge", false,                  \
	        kind##_ndirty_nonhuge),                                        \
	    LEAF("hpa_shard." #kind ".ndirty_huge", false, kind##_ndirty_huge)
#define LEAF_NONFULL(name)                                                     \
	{"hpa_shard.nonfull_slabs", PSSET_NPSIZES, #name, false,               \
	    leaf_get_nonfull_##name}
#define LEAF_ALLOC(name)                                                       \
	{"hpa_shard.alloc", SEC_MAX_NALLOCS + 1, #name, true,                  \
	    leaf_get_alloc_##name}

static const hpa_leaf_t hpa_leaves[] = {
    LEAF("hpa_sec_bytes", false, sec_bytes),
    LEAF("hpa_sec_hits", false, sec_hits),
    LEAF("hpa_sec_misses", false, sec_misses),
    LEAF("hpa_sec_dalloc_noflush", false, sec_dalloc_noflush),
    LEAF("hpa_sec_dalloc_flush", false, sec_dalloc_flush),
    LEAF("hpa_sec_overfills", false, sec_overfills),
    LEAF("hpa_shard.npageslabs", false, npageslabs),
    LEAF("hpa_shard.nactive", false, nactive),
    LEAF("hpa_shard.ndirty", false, ndirty),
    LEAF("hpa_shard.npurge_passes", true, npurge_passes),
    LEAF("hpa_shard.npurges", true, npurges),
    LEAF("hpa_shard.nhugifies", true, nhugifies),
    LEAF("hpa_shard.nhugify_failures", true, nhugify_failures),
    LEAF("hpa_shard.ndehugifies", true, ndehugifies),
    LEAF_SLAB(slabs),
    LEAF_SLAB(full_slabs),
    LEAF_SLAB(empty_slabs),
    LEAF_NONFULL(npageslabs_nonhuge),
    LEAF_NONFULL(npageslabs_huge),
    LEAF_NONFULL(nactive_nonhuge),
    LEAF_NONFULL(nactive_huge),
    LEAF_NONFULL(ndirty_nonhuge),
    LEAF_NONFULL(ndirty_huge),
    LEAF_ALLOC(min_extents),
    LEAF_ALLOC(max_extents),
    LEAF_ALLOC(extents),
    LEAF_ALLOC(ps),
    LEAF_ALLOC(pages_per_ps),
    LEAF_ALLOC(extents_per_ps),
};

static uint64_t
read_u64(const char *name) {
	uint64_t v = 0;
	size_t   sz = sizeof(v);
	expect_d_eq(mallctl(name, &v, &sz, NULL, 0), 0, "%s unreadable", name);
	return v;
}

static size_t
read_size(const char *name) {
	size_t v = 0;
	size_t sz = sizeof(v);
	expect_d_eq(mallctl(name, &v, &sz, NULL, 0), 0, "%s unreadable", name);
	return v;
}

static uint64_t
read_leaf(const char *prefix, const hpa_leaf_t *leaf, unsigned j) {
	char name[256];
	if (leaf->group == NULL) {
		malloc_snprintf(
		    name, sizeof(name), "%s.%s", prefix, leaf->name);
	} else {
		malloc_snprintf(name, sizeof(name), "%s.%s.%u.%s", prefix,
		    leaf->group, j, leaf->name);
	}
	return leaf->u64 ? read_u64(name) : (uint64_t)read_size(name);
}

static void
epoch_advance(void) {
	uint64_t epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");
}

/* Fills arenas[] with the "stats.arenas.<i>" prefixes of initialized arenas. */
#define MAX_ARENAS_CHECKED 64
static unsigned
initialized_arenas(char arenas[][32]) {
	unsigned narenas;
	size_t   sz = sizeof(narenas);
	expect_d_eq(mallctl("arenas.narenas", &narenas, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	unsigned n = 0;
	for (unsigned i = 0; i < narenas; i++) {
		char name[64];
		bool initialized;
		sz = sizeof(initialized);
		malloc_snprintf(name, sizeof(name), "arena.%u.initialized", i);
		expect_d_eq(mallctl(name, &initialized, &sz, NULL, 0), 0,
		    "Unexpected mallctl() failure");
		if (initialized) {
			assert_u_lt(n, MAX_ARENAS_CHECKED, "too many arenas");
			malloc_snprintf(arenas[n], 32, "stats.arenas.%u", i);
			n++;
		}
	}
	return n;
}

/*
 * Puts HPA memory in the arenas, once: allocations of several sizes in an
 * automatic and a manual arena, half of them freed again.  The rest stay
 * allocated so the statistics keep describing them.
 */
#define NPTRS 48
static void *ptrs[NPTRS];
static void
arenas_hpa_traffic(void) {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;

	unsigned manual;
	size_t   sz = sizeof(manual);
	expect_d_eq(mallctl("arenas.create", &manual, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	static const size_t sizes[] = {1, 4, 16};
	for (unsigned i = 0; i < NPTRS; i++) {
		int flags = MALLOCX_TCACHE_NONE
		    | (i % 2 == 0 ? MALLOCX_ARENA(0) : MALLOCX_ARENA(manual));
		ptrs[i] = mallocx(sizes[i % 3] * PAGE, flags);
		expect_ptr_not_null(ptrs[i], "Unexpected mallocx() failure");
	}
	for (unsigned i = 0; i < NPTRS; i += 2) {
		dallocx(ptrs[i], MALLOCX_TCACHE_NONE);
		ptrs[i] = NULL;
	}
}

static void
merged_prefix(char *buf, size_t len) {
	malloc_snprintf(buf, len, "stats.arenas.%u", MALLCTL_ARENAS_ALL);
}

/* Makes sure the checks below compare real numbers, not only zeroes. */
static void
assert_arenas_hold_hpa_memory(const char *merged) {
	char name[64];
	malloc_snprintf(name, sizeof(name), "%s.hpa_shard.npageslabs", merged);
	assert_zu_ne(
	    read_size(name), 0, "the arenas should hold HPA pageslabs");
}

/* Formats a leaf's name for messages: "<name>" or "<group>.<j>.<name>". */
static const char *
leaf_label(const hpa_leaf_t *leaf, unsigned j, char *buf, size_t len) {
	if (leaf->group == NULL) {
		return leaf->name;
	}
	malloc_snprintf(buf, len, "%s.%u.%s", leaf->group, j, leaf->name);
	return buf;
}

TEST_BEGIN(test_sets_leaves_match_sets) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_hpa);

	arenas_hpa_traffic();
	epoch_advance();

	hpa_shard_stats_t sets;
	memset(&sets, 0, sizeof(sets));
	hpa_shard_sets_stats_merge(
	    tsd_tsdn(tsd_fetch()), &hpa_shard_sets_global, &sets);

	char merged[32];
	merged_prefix(merged, sizeof(merged));
	assert_arenas_hold_hpa_memory(merged);

	for (size_t l = 0; l < ARRAY_COUNT(hpa_leaves); l++) {
		const hpa_leaf_t *leaf = &hpa_leaves[l];
		unsigned          n = leaf->group == NULL ? 1 : leaf->nindices;
		for (unsigned j = 0; j < n; j++) {
			char label[96];
			uint64_t got = read_leaf(
			    "stats.hpa_shard_sets", leaf, j);
			expect_u64_eq(got, leaf->get(&sets, j),
			    "%s differs from the sets",
			    leaf_label(leaf, j, label, sizeof(label)));
		}
	}
}
TEST_END

/* Whether any shard of the global sets has been created. */
static bool
sets_have_shards(void) {
	hpa_shard_sets_t *sets = &hpa_shard_sets_global;
	for (unsigned i = 0; i < sets->nsets; i++) {
		for (unsigned j = 0; j < sets->sets[i].nshards; j++) {
			if (hpa_shard_set_shard_from_id(&sets->sets[i], j)
			    != NULL) {
				return true;
			}
		}
	}
	return false;
}

TEST_BEGIN(test_merged_is_arenas_plus_sets) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_hpa);

	arenas_hpa_traffic();
	epoch_advance();

	char     arenas[MAX_ARENAS_CHECKED][32];
	unsigned narenas = initialized_arenas(arenas);
	char     merged[32];
	merged_prefix(merged, sizeof(merged));
	assert_arenas_hold_hpa_memory(merged);

	/* Every HPA leaf. */
	for (size_t l = 0; l < ARRAY_COUNT(hpa_leaves); l++) {
		const hpa_leaf_t *leaf = &hpa_leaves[l];
		unsigned          n = leaf->group == NULL ? 1 : leaf->nindices;
		for (unsigned j = 0; j < n; j++) {
			uint64_t sum = read_leaf(
			    "stats.hpa_shard_sets", leaf, j);
			for (unsigned a = 0; a < narenas; a++) {
				sum += read_leaf(arenas[a], leaf, j);
			}
			char label[96];
			expect_u64_eq(read_leaf(merged, leaf, j), sum,
			    "merged %s is not the arenas plus the sets",
			    leaf_label(leaf, j, label, sizeof(label)));
		}
	}

	/*
	 * Active pages are charged to arenas; the sets add their dirty pages to
	 * the merged arena's dirty pages and resident bytes.
	 */
	size_t sets_ndirty = hpa_shard_sets_ndirty(&hpa_shard_sets_global);
	size_t pactive = 0, pdirty = 0, resident = 0;
	for (unsigned a = 0; a < narenas; a++) {
		char name[64];
		malloc_snprintf(name, sizeof(name), "%s.pactive", arenas[a]);
		pactive += read_size(name);
		malloc_snprintf(name, sizeof(name), "%s.pdirty", arenas[a]);
		pdirty += read_size(name);
		malloc_snprintf(name, sizeof(name), "%s.resident", arenas[a]);
		resident += read_size(name);
	}
	char name[64];
	malloc_snprintf(name, sizeof(name), "%s.pactive", merged);
	expect_zu_eq(read_size(name), pactive, "merged pactive");
	malloc_snprintf(name, sizeof(name), "%s.pdirty", merged);
	expect_zu_eq(read_size(name), pdirty + sets_ndirty, "merged pdirty");
	malloc_snprintf(name, sizeof(name), "%s.resident", merged);
	size_t merged_resident = read_size(name);
	expect_zu_eq(merged_resident, resident + (sets_ndirty << LG_PAGE),
	    "merged resident");
	expect_zu_eq(read_size("stats.resident"), merged_resident,
	    "stats.resident should be the merged arena's");

	/*
	 * The merged HPA mutex rows: counts add up and maxima take the
	 * largest.  Only exact while the sets have no shards, whose counters
	 * the stats refresh itself changes; the commit that boots the sets
	 * checks the fold with them.
	 */
	if (sets_have_shards()) {
		return;
	}
	static const char *const mutexes[] = {
	    "hpa_shard", "hpa_shard_grow", "hpa_sec"};
	static const char *const counts[] = {"num_ops", "num_wait",
	    "num_spin_acq", "num_owner_switch", "total_wait_time"};
	for (size_t m = 0; m < ARRAY_COUNT(mutexes); m++) {
		for (size_t c = 0; c < ARRAY_COUNT(counts); c++) {
			uint64_t sum = 0;
			for (unsigned a = 0; a < narenas; a++) {
				malloc_snprintf(name, sizeof(name),
				    "%s.mutexes.%s.%s", arenas[a], mutexes[m],
				    counts[c]);
				sum += read_u64(name);
			}
			malloc_snprintf(name, sizeof(name), "%s.mutexes.%s.%s",
			    merged, mutexes[m], counts[c]);
			expect_u64_eq(read_u64(name), sum,
			    "merged %s.%s is not the arenas'", mutexes[m],
			    counts[c]);
		}
		uint64_t max_wait = 0;
		uint32_t max_thds = 0;
		for (unsigned a = 0; a < narenas; a++) {
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_wait_time", arenas[a],
			    mutexes[m]);
			uint64_t wait = read_u64(name);
			max_wait = wait > max_wait ? wait : max_wait;
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_num_thds", arenas[a],
			    mutexes[m]);
			uint32_t thds;
			size_t   sz = sizeof(thds);
			expect_d_eq(mallctl(name, &thds, &sz, NULL, 0), 0,
			    "%s unreadable", name);
			max_thds = thds > max_thds ? thds : max_thds;
		}
		malloc_snprintf(name, sizeof(name),
		    "%s.mutexes.%s.max_wait_time", merged, mutexes[m]);
		expect_u64_eq(read_u64(name), max_wait,
		    "merged %s.max_wait_time is not the arenas' largest",
		    mutexes[m]);
		malloc_snprintf(name, sizeof(name),
		    "%s.mutexes.%s.max_num_thds", merged, mutexes[m]);
		uint32_t merged_thds;
		size_t   sz = sizeof(merged_thds);
		expect_d_eq(mallctl(name, &merged_thds, &sz, NULL, 0), 0,
		    "%s unreadable", name);
		expect_u32_eq(merged_thds, max_thds,
		    "merged %s.max_num_thds is not the arenas' largest",
		    mutexes[m]);
	}
}
TEST_END

int
main(void) {
	if (config_stats && hpa_supported()) {
		opt_hpa = true;
	}
	return test_no_reentrancy(
	    test_sets_leaves_match_sets, test_merged_is_arenas_plus_sets);
}
