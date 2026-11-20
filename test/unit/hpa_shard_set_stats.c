#include "test/jemalloc_test.h"
#include "test/hpa_shard_sets.h"

#include "jemalloc/internal/ctl.h"

/*
 * Checks what ctl reports for the global HPA shard sets, which malloc_init
 * boots from the options in the .sh, and which hold memory from the tests and
 * from arena 0, while a manual arena's own HPA shard holds memory too: each
 * shard as the shard's own statistics, each set as its shards added up, and
 * the merged arena as the arenas plus the sets.  The checks compare ctl with
 * the statistics it is built from, or with other ctl figures, so they hold
 * whatever the HPA's policy; background threads are off so that nothing
 * changes between reads.
 */

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/*
 * An HPA leaf, named as below "stats.arenas.<i>",
 * "stats.hpa_shard_sets.<i>" and "stats.hpa_shard_sets.<i>.shards.<j>":
 * "<name>" if group is NULL, else "<group>.<k>.<name>" for k < nindices.
 */
typedef struct hpa_leaf_s {
	const char *group;
	unsigned    nindices;
	const char *name;
	bool        u64;
	uint64_t (*get)(const hpa_shard_stats_t *s, unsigned k);
} hpa_leaf_t;

#define LEAF_GET(id, expr)                                                     \
	static uint64_t leaf_get_##id(                                         \
	    const hpa_shard_stats_t *s, unsigned k) {                          \
		(void)k;                                                       \
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
LEAF_GET(
    nonfull_npageslabs_nonhuge, s->psset_stats.nonfull_slabs[k][0].npageslabs)
LEAF_GET(nonfull_npageslabs_huge, s->psset_stats.nonfull_slabs[k][1].npageslabs)
LEAF_GET(nonfull_nactive_nonhuge, s->psset_stats.nonfull_slabs[k][0].nactive)
LEAF_GET(nonfull_nactive_huge, s->psset_stats.nonfull_slabs[k][1].nactive)
LEAF_GET(nonfull_ndirty_nonhuge, s->psset_stats.nonfull_slabs[k][0].ndirty)
LEAF_GET(nonfull_ndirty_huge, s->psset_stats.nonfull_slabs[k][1].ndirty)
LEAF_GET(alloc_min_extents, s->nonderived_stats.hpa_alloc_min_extents[k])
LEAF_GET(alloc_max_extents, s->nonderived_stats.hpa_alloc_max_extents[k])
LEAF_GET(alloc_extents, s->nonderived_stats.hpa_alloc_extents[k])
LEAF_GET(alloc_ps, s->nonderived_stats.hpa_alloc_ps[k])
LEAF_GET(alloc_pages_per_ps, s->nonderived_stats.hpa_alloc_pages_per_ps[k])
LEAF_GET(alloc_extents_per_ps, s->nonderived_stats.hpa_alloc_extents_per_ps[k])

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

/* The HPA shard's mutexes, and their counters that add up when merged. */
static const char *const mutexes[] = {"hpa_shard", "hpa_shard_grow", "hpa_sec"};
static const char *const mutex_counts[] = {"num_ops", "num_wait",
    "num_spin_acq", "num_owner_switch", "total_wait_time"};

static uint64_t
read_u64(const char *name) {
	uint64_t v = 0;
	size_t   sz = sizeof(v);
	expect_d_eq(mallctl(name, &v, &sz, NULL, 0), 0, "%s unreadable", name);
	return v;
}

static uint32_t
read_u32(const char *name) {
	uint32_t v = 0;
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

static unsigned
read_unsigned(const char *name) {
	unsigned v = 0;
	size_t   sz = sizeof(v);
	expect_d_eq(mallctl(name, &v, &sz, NULL, 0), 0, "%s unreadable", name);
	return v;
}

static bool
read_bool(const char *name) {
	bool   v = false;
	size_t sz = sizeof(v);
	expect_d_eq(mallctl(name, &v, &sz, NULL, 0), 0, "%s unreadable", name);
	return v;
}

static uint64_t
read_leaf(const char *prefix, const hpa_leaf_t *leaf, unsigned k) {
	char name[256];
	if (leaf->group == NULL) {
		malloc_snprintf(
		    name, sizeof(name), "%s.%s", prefix, leaf->name);
	} else {
		malloc_snprintf(name, sizeof(name), "%s.%s.%u.%s", prefix,
		    leaf->group, k, leaf->name);
	}
	return leaf->u64 ? read_u64(name) : (uint64_t)read_size(name);
}

/* Formats a leaf's name for messages: "<name>" or "<group>.<k>.<name>". */
static const char *
leaf_label(const hpa_leaf_t *leaf, unsigned k, char *buf, size_t len) {
	if (leaf->group == NULL) {
		return leaf->name;
	}
	malloc_snprintf(buf, len, "%s.%u.%s", leaf->group, k, leaf->name);
	return buf;
}

static unsigned
leaf_nindices(const hpa_leaf_t *leaf) {
	return leaf->group == NULL ? 1 : leaf->nindices;
}

static void
epoch_advance(void) {
	uint64_t epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)), 0,
	    "Unexpected mallctl() failure");
}

static void
set_prefix(char *buf, size_t len, unsigned i) {
	malloc_snprintf(buf, len, "stats.hpa_shard_sets.%u", i);
}

static void
shard_prefix(char *buf, size_t len, unsigned i, unsigned j) {
	malloc_snprintf(buf, len, "stats.hpa_shard_sets.%u.shards.%u", i, j);
}

/* The ways ctl looks up a slot, which must all agree on whether it exists. */
typedef enum {
	slot_lookup_by_name,
	slot_lookup_by_mib,
	/* The slot's index is resolved below the mib of its set. */
	slot_lookup_below_set_mib,
	slot_nlookups
} slot_lookup_t;

/* Reads whether slot j of set i has a shard, returning the mallctl result. */
static int
slot_initialized(
    slot_lookup_t lookup, unsigned i, unsigned j, bool *initialized) {
	size_t mib[CTL_MAX_DEPTH];
	size_t miblen = CTL_MAX_DEPTH;
	size_t sz = sizeof(*initialized);
	char   name[128];
	switch (lookup) {
	case slot_lookup_by_name:
		malloc_snprintf(name, sizeof(name),
		    "stats.hpa_shard_sets.%u.shards.%u.initialized", i, j);
		return mallctl(name, initialized, &sz, NULL, 0);
	case slot_lookup_by_mib:
		assert_d_eq(mallctlnametomib(
		                "stats.hpa_shard_sets.0.shards.0.initialized", mib,
		                &miblen),
		    0, "Unexpected mallctlnametomib() failure");
		mib[2] = i;
		mib[4] = j;
		return mallctlbymib(mib, miblen, initialized, &sz, NULL, 0);
	default:
		assert_d_eq(
		    mallctlnametomib("stats.hpa_shard_sets.0", mib, &miblen), 0,
		    "Unexpected mallctlnametomib() failure");
		mib[2] = i;
		miblen = CTL_MAX_DEPTH;
		malloc_snprintf(name, sizeof(name), "shards.%u.initialized", j);
		return ctl_bymibname(tsd_fetch(), mib, 3, name, &miblen,
		    initialized, &sz, NULL, 0);
	}
}

/*
 * With the .sh's layout, set 0 serves the first two bands, from two slots, and
 * set 1 every larger size, from three.  The tests create the shards in slot 1
 * of set 0, and slots 0 and 2 of set 1.  The test's own allocations come from
 * the arena of the test's thread, by default arena 0, which uses slot 0 of
 * each set, so slot 1 of set 1 stays without a shard; the checks compare ctl
 * with whatever shard each slot has.
 */
#define NSETS 2
#define NSHARDS 3
static const unsigned sets_nshards[NSETS] = {2, 3};
static const unsigned shard_set_ind[NSHARDS] = {0, 1, 1};
static const unsigned shard_slot[NSHARDS] = {1, 0, 2};
static hpa_shard_t   *shards[NSHARDS];

/*
 * Whether the sets were booted as the .sh lays them out, which the tests need;
 * under another layout, e.g. from MALLOC_CONF, they skip.
 */
static bool
sets_as_configured(void) {
	if (hpa_shard_sets_global.nsets != NSETS) {
		return false;
	}
	for (unsigned i = 0; i < NSETS; i++) {
		if (hpa_shard_sets_global.sets[i].nshards != sets_nshards[i]) {
			return false;
		}
	}
	return true;
}

/* The shard in slot j of set i, whatever created it, or NULL. */
static hpa_shard_t *
slot_shard(unsigned i, unsigned j) {
	return (hpa_shard_t *)atomic_load_p(
	    &hpa_shard_sets_global.sets[i].shards[j], ATOMIC_ACQUIRE);
}

/*
 * Creates the shards once, and puts memory in each: a page kept allocated, and
 * a page freed again, which the SEC takes.  Flushing the SEC then turns the
 * pages it holds into dirty pages, which stay as purging is off (see the .sh).
 */
static void
sets_setup(void) {
	static bool done = false;
	if (done) {
		return;
	}
	done = true;

	assert_u_eq(hpa_shard_sets_global.nsets, NSETS, "unexpected layout");
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	for (unsigned s = 0; s < NSHARDS; s++) {
		shards[s] = test_hpa_shard_sets_shard(
		    shard_set_ind[s], shard_slot[s]);
		test_hpa_shard_alloc(shards[s], PAGE);
		test_hpa_shard_dalloc(
		    shards[s], test_hpa_shard_alloc(shards[s], PAGE));
		hpa_shard_flush(tsdn, shards[s]);
	}

	/*
	 * Uncontended mutexes record no waits, so give the shards' mtx distinct
	 * maxima, larger in later slots, for the checks of maxima to compare.
	 */
	for (unsigned s = 0; s < NSHARDS; s++) {
		malloc_mutex_t *mtx = &shards[s]->mtx;
		malloc_mutex_lock(tsdn, mtx);
		nstime_init(&mtx->prof_data.max_wait_time,
		    (uint64_t)(s + 1) * 1000 * 1000);
		mtx->prof_data.max_n_thds = s + 2;
		malloc_mutex_unlock(tsdn, mtx);
	}
}

/* The shard the tests created in slot j of set i, if any. */
static hpa_shard_t *
created_shard(unsigned i, unsigned j) {
	for (unsigned s = 0; s < NSHARDS; s++) {
		if (shard_set_ind[s] == i && shard_slot[s] == j) {
			return shards[s];
		}
	}
	return NULL;
}

/*
 * Puts HPA memory in a manual arena's own HPA shard and, through arena 0, in
 * the sets, once: allocations of several sizes, half of them freed again.  The
 * rest stay allocated so the statistics keep describing them.
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

TEST_BEGIN(test_layout) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!sets_as_configured());

	sets_setup();
	epoch_advance();

	/* The effective mapping: each set's sizes and slots. */
	hpa_shard_sets_t *sets = &hpa_shard_sets_global;
	expect_u_eq(read_unsigned("stats.nhpa_shard_sets"), sets->nsets,
	    "every set should be reported");
	for (unsigned i = 0; i < sets->nsets; i++) {
		char prefix[64], name[128];
		set_prefix(prefix, sizeof(prefix), i);
		malloc_snprintf(name, sizeof(name), "%s.size_max", prefix);
		expect_zu_eq(read_size(name), sets->sets[i].size_max,
		    "set %u's size_max", i);
		malloc_snprintf(name, sizeof(name), "%s.nshards", prefix);
		expect_u_eq(read_unsigned(name), sets->sets[i].nshards,
		    "set %u's nshards", i);

		/* Whether each slot has a shard, and no slot past the last. */
		unsigned nshards = sets->sets[i].nshards;
		for (unsigned j = 0; j <= nshards; j++) {
			for (slot_lookup_t lookup = 0; lookup < slot_nlookups;
			    lookup++) {
				bool initialized = false;
				int  ret = slot_initialized(
				    lookup, i, j, &initialized);
				if (j == nshards) {
					expect_d_eq(ret, ENOENT,
					    "set %u: no slot past the last, "
					    "lookup %d",
					    i, lookup);
					continue;
				}
				expect_d_eq(ret, 0,
				    "set %u, slot %u: lookup %d failed", i, j,
				    lookup);
				expect_b_eq(initialized,
				    slot_shard(i, j) != NULL,
				    "set %u, slot %u: initialized, lookup %d", i,
				    j, lookup);
			}
		}
	}
	expect_zu_eq(read_size("stats.hpa_shard_sets.0.size_max"),
	    hpa_shard_set_band_max[1], "set 0 should end with band 1");

	/* No set past the last, by name, by mib, or below the sets' mib. */
	char   name[128];
	size_t v;
	size_t sz = sizeof(v);
	malloc_snprintf(name, sizeof(name), "stats.hpa_shard_sets.%u.size_max",
	    sets->nsets);
	expect_d_eq(
	    mallctl(name, &v, &sz, NULL, 0), ENOENT, "no set past the last");
	size_t mib[CTL_MAX_DEPTH];
	size_t miblen = CTL_MAX_DEPTH;
	assert_d_eq(
	    mallctlnametomib("stats.hpa_shard_sets.0.size_max", mib, &miblen),
	    0, "Unexpected mallctlnametomib() failure");
	mib[2] = sets->nsets;
	expect_d_eq(mallctlbymib(mib, miblen, &v, &sz, NULL, 0), ENOENT,
	    "no set past the last, by mib");
	miblen = CTL_MAX_DEPTH;
	malloc_snprintf(name, sizeof(name), "%u.size_max", sets->nsets);
	expect_d_eq(ctl_bymibname(tsd_fetch(), mib, 2, name, &miblen, &v, &sz,
	                NULL, 0),
	    ENOENT, "no set past the last, below the sets' mib");

	/* No bucket past the last, in a set or a shard. */
	static const char *const prefixes[] = {
	    "stats.hpa_shard_sets.0", "stats.hpa_shard_sets.0.shards.1"};
	for (size_t p = 0; p < ARRAY_COUNT(prefixes); p++) {
		uint64_t bucket;
		sz = sizeof(bucket);
		malloc_snprintf(name, sizeof(name),
		    "%s.hpa_shard.nonfull_slabs.%u.npageslabs_huge",
		    prefixes[p], (unsigned)PSSET_NPSIZES);
		expect_d_eq(mallctl(name, &bucket, &sz, NULL, 0), ENOENT,
		    "%s: no nonfull_slabs bucket past the last", prefixes[p]);
		malloc_snprintf(name, sizeof(name), "%s.hpa_shard.alloc.%u.extents",
		    prefixes[p], (unsigned)SEC_MAX_NALLOCS + 1);
		expect_d_eq(mallctl(name, &bucket, &sz, NULL, 0), ENOENT,
		    "%s: no alloc bucket past the last", prefixes[p]);
	}
}
TEST_END

TEST_BEGIN(test_shard_leaves) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!sets_as_configured());

	sets_setup();
	epoch_advance();

	tsdn_t           *tsdn = tsd_tsdn(tsd_fetch());
	hpa_shard_sets_t *sets = &hpa_shard_sets_global;
	for (unsigned i = 0; i < sets->nsets; i++) {
		for (unsigned j = 0; j < sets->sets[i].nshards; j++) {
			/* A slot without a shard reads as zeroes. */
			hpa_shard_stats_t oracle;
			memset(&oracle, 0, sizeof(oracle));
			hpa_shard_t *shard = slot_shard(i, j);
			if (shard != NULL) {
				hpa_shard_stats_merge(tsdn, shard, &oracle);
			}
			if (created_shard(i, j) != NULL) {
				assert_zu_ne(
				    oracle.psset_stats.merged.npageslabs, 0,
				    "each shard created should hold memory");
			}

			char prefix[96];
			shard_prefix(prefix, sizeof(prefix), i, j);
			for (size_t l = 0; l < ARRAY_COUNT(hpa_leaves); l++) {
				const hpa_leaf_t *leaf = &hpa_leaves[l];
				for (unsigned k = 0; k < leaf_nindices(leaf);
				    k++) {
					char label[96];
					expect_u64_eq(
					    read_leaf(prefix, leaf, k),
					    leaf->get(&oracle, k),
					    "set %u, slot %u: %s", i, j,
					    leaf_label(
					        leaf, k, label, sizeof(label)));
				}
			}

			/* A shard's mutex was taken, at least to read it. */
			char name[160];
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.hpa_shard.num_ops", prefix);
			if (shard != NULL) {
				expect_u64_gt(read_u64(name), 0,
				    "set %u, slot %u: num_ops", i, j);
			} else {
				expect_u64_eq(read_u64(name), 0,
				    "set %u, slot %u: num_ops", i, j);
			}
		}
	}
}
TEST_END

/* Sums leaf k over the shards of set i, from ctl. */
static uint64_t
shards_sum(unsigned i, unsigned nshards, const hpa_leaf_t *leaf, unsigned k) {
	uint64_t sum = 0;
	for (unsigned j = 0; j < nshards; j++) {
		char prefix[96];
		shard_prefix(prefix, sizeof(prefix), i, j);
		sum += read_leaf(prefix, leaf, k);
	}
	return sum;
}

TEST_BEGIN(test_set_leaves) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!sets_as_configured());

	sets_setup();
	epoch_advance();

	hpa_shard_sets_t *sets = &hpa_shard_sets_global;
	for (unsigned i = 0; i < sets->nsets; i++) {
		unsigned nshards = sets->sets[i].nshards;
		char     prefix[64];
		set_prefix(prefix, sizeof(prefix), i);
		for (size_t l = 0; l < ARRAY_COUNT(hpa_leaves); l++) {
			const hpa_leaf_t *leaf = &hpa_leaves[l];
			for (unsigned k = 0; k < leaf_nindices(leaf); k++) {
				char label[96];
				expect_u64_eq(read_leaf(prefix, leaf, k),
				    shards_sum(i, nshards, leaf, k),
				    "set %u: %s is not its shards' sum", i,
				    leaf_label(leaf, k, label, sizeof(label)));
			}
		}

		/* The set's mutexes: counts add up, maxima take the largest. */
		for (size_t m = 0; m < ARRAY_COUNT(mutexes); m++) {
			char name[160];
			for (size_t c = 0; c < ARRAY_COUNT(mutex_counts); c++) {
				uint64_t sum = 0;
				for (unsigned j = 0; j < nshards; j++) {
					malloc_snprintf(name, sizeof(name),
					    "stats.hpa_shard_sets.%u.shards.%u."
					    "mutexes.%s.%s",
					    i, j, mutexes[m], mutex_counts[c]);
					sum += read_u64(name);
				}
				malloc_snprintf(name, sizeof(name),
				    "%s.mutexes.%s.%s", prefix, mutexes[m],
				    mutex_counts[c]);
				expect_u64_eq(read_u64(name), sum,
				    "set %u: %s.%s is not its shards' sum", i,
				    mutexes[m], mutex_counts[c]);
			}
			uint64_t max_wait = 0;
			uint32_t max_thds = 0;
			for (unsigned j = 0; j < nshards; j++) {
				malloc_snprintf(name, sizeof(name),
				    "stats.hpa_shard_sets.%u.shards.%u.mutexes."
				    "%s.max_wait_time",
				    i, j, mutexes[m]);
				uint64_t wait = read_u64(name);
				max_wait = wait > max_wait ? wait : max_wait;
				malloc_snprintf(name, sizeof(name),
				    "stats.hpa_shard_sets.%u.shards.%u.mutexes."
				    "%s.max_num_thds",
				    i, j, mutexes[m]);
				uint32_t thds = read_u32(name);
				max_thds = thds > max_thds ? thds : max_thds;
			}
			if (strcmp(mutexes[m], "hpa_shard") == 0) {
				expect_u64_gt(max_wait, 0,
				    "set %u: the seeded maxima should show", i);
			}
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_wait_time", prefix, mutexes[m]);
			expect_u64_eq(read_u64(name), max_wait,
			    "set %u: %s.max_wait_time", i, mutexes[m]);
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_num_thds", prefix, mutexes[m]);
			expect_u32_eq(read_u32(name), max_thds,
			    "set %u: %s.max_num_thds", i, mutexes[m]);
		}
	}
}
TEST_END

/* Fills arenas[] with the "stats.arenas.<i>" prefixes of initialized arenas. */
#define MAX_ARENAS_CHECKED 64
static unsigned
initialized_arenas(char arenas[][32]) {
	unsigned narenas = read_unsigned("arenas.narenas");
	unsigned n = 0;
	for (unsigned i = 0; i < narenas; i++) {
		char name[64];
		malloc_snprintf(name, sizeof(name), "arena.%u.initialized", i);
		if (read_bool(name)) {
			assert_u_lt(n, MAX_ARENAS_CHECKED, "too many arenas");
			malloc_snprintf(arenas[n], 32, "stats.arenas.%u", i);
			n++;
		}
	}
	return n;
}

/* Fills sets[] with the "stats.hpa_shard_sets.<i>" prefixes. */
static unsigned
reported_sets(char sets[][32]) {
	unsigned nsets = read_unsigned("stats.nhpa_shard_sets");
	assert_u_le(nsets, HPA_SHARD_SET_NBANDS, "too many sets");
	for (unsigned i = 0; i < nsets; i++) {
		set_prefix(sets[i], 32, i);
	}
	return nsets;
}

TEST_BEGIN(test_merged_is_arenas_plus_sets) {
	test_skip_if(!config_stats);
	test_skip_if(!opt_hpa);
	test_skip_if(!sets_as_configured());

	sets_setup();
	arenas_hpa_traffic();
	epoch_advance();

	/* The arenas and the sets: the parts of the merged arena. */
	char     parts[MAX_ARENAS_CHECKED + HPA_SHARD_SET_NBANDS][32];
	unsigned narenas = initialized_arenas(parts);
	unsigned nparts = narenas + reported_sets(&parts[narenas]);
	assert_u_gt(nparts, narenas, "the sets should be reported");
	char merged[32];
	malloc_snprintf(
	    merged, sizeof(merged), "stats.arenas.%u", MALLCTL_ARENAS_ALL);

	/* Makes sure the checks compare real numbers, not only zeroes. */
	char   name[160];
	size_t arenas_npageslabs = 0;
	for (unsigned a = 0; a < narenas; a++) {
		malloc_snprintf(
		    name, sizeof(name), "%s.hpa_shard.npageslabs", parts[a]);
		arenas_npageslabs += read_size(name);
	}
	assert_zu_gt(
	    arenas_npageslabs, 0, "the arenas should hold HPA pageslabs too");

	/* Every HPA leaf. */
	for (size_t l = 0; l < ARRAY_COUNT(hpa_leaves); l++) {
		const hpa_leaf_t *leaf = &hpa_leaves[l];
		for (unsigned k = 0; k < leaf_nindices(leaf); k++) {
			uint64_t sum = 0;
			for (unsigned p = 0; p < nparts; p++) {
				sum += read_leaf(parts[p], leaf, k);
			}
			char label[96];
			expect_u64_eq(read_leaf(merged, leaf, k), sum,
			    "merged %s is not the arenas plus the sets",
			    leaf_label(leaf, k, label, sizeof(label)));
		}
	}

	/*
	 * Active pages are charged to arenas; the sets add their dirty pages,
	 * with the pages their SECs hold, to the merged arena's dirty pages and
	 * resident bytes.
	 */
	size_t sets_ndirty = 0;
	for (unsigned p = narenas; p < nparts; p++) {
		malloc_snprintf(
		    name, sizeof(name), "%s.hpa_shard.ndirty", parts[p]);
		sets_ndirty += read_size(name);
		malloc_snprintf(
		    name, sizeof(name), "%s.hpa_sec_bytes", parts[p]);
		sets_ndirty += read_size(name) >> LG_PAGE;
	}
	assert_zu_ne(sets_ndirty, 0, "the sets should hold dirty pages");
	size_t pactive = 0, pdirty = 0, resident = 0;
	for (unsigned a = 0; a < narenas; a++) {
		malloc_snprintf(name, sizeof(name), "%s.pactive", parts[a]);
		pactive += read_size(name);
		malloc_snprintf(name, sizeof(name), "%s.pdirty", parts[a]);
		pdirty += read_size(name);
		malloc_snprintf(name, sizeof(name), "%s.resident", parts[a]);
		resident += read_size(name);
	}
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

	/* The merged HPA mutexes: counts add up, maxima take the largest. */
	for (size_t m = 0; m < ARRAY_COUNT(mutexes); m++) {
		for (size_t c = 0; c < ARRAY_COUNT(mutex_counts); c++) {
			uint64_t sum = 0;
			for (unsigned p = 0; p < nparts; p++) {
				malloc_snprintf(name, sizeof(name),
				    "%s.mutexes.%s.%s", parts[p], mutexes[m],
				    mutex_counts[c]);
				sum += read_u64(name);
			}
			malloc_snprintf(name, sizeof(name), "%s.mutexes.%s.%s",
			    merged, mutexes[m], mutex_counts[c]);
			expect_u64_eq(read_u64(name), sum,
			    "merged %s.%s is not the arenas' plus the sets'",
			    mutexes[m], mutex_counts[c]);
		}
		uint64_t max_wait = 0;
		uint32_t max_thds = 0;
		for (unsigned p = 0; p < nparts; p++) {
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_wait_time", parts[p],
			    mutexes[m]);
			uint64_t wait = read_u64(name);
			max_wait = wait > max_wait ? wait : max_wait;
			malloc_snprintf(name, sizeof(name),
			    "%s.mutexes.%s.max_num_thds", parts[p], mutexes[m]);
			uint32_t thds = read_u32(name);
			max_thds = thds > max_thds ? thds : max_thds;
		}
		malloc_snprintf(name, sizeof(name),
		    "%s.mutexes.%s.max_wait_time", merged, mutexes[m]);
		expect_u64_eq(read_u64(name), max_wait,
		    "merged %s.max_wait_time is not the largest", mutexes[m]);
		malloc_snprintf(name, sizeof(name),
		    "%s.mutexes.%s.max_num_thds", merged, mutexes[m]);
		expect_u32_eq(read_u32(name), max_thds,
		    "merged %s.max_num_thds is not the largest", mutexes[m]);
	}
}
TEST_END

/* stats.mutexes.reset clears the mutex statistics of the sets' shards too. */
TEST_BEGIN(test_mutexes_reset) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!sets_as_configured());

	sets_setup();
	epoch_advance();
	/* The maxima sets_setup() seeded, and the locks the traffic took. */
	uint64_t ops_before[NSHARDS];
	for (unsigned s = 0; s < NSHARDS; s++) {
		char name[160];
		malloc_snprintf(name, sizeof(name),
		    "stats.hpa_shard_sets.%u.shards.%u.mutexes.hpa_shard.num_ops",
		    shard_set_ind[s], shard_slot[s]);
		ops_before[s] = read_u64(name);
		malloc_snprintf(name, sizeof(name),
		    "stats.hpa_shard_sets.%u.shards.%u.mutexes.hpa_shard."
		    "max_wait_time",
		    shard_set_ind[s], shard_slot[s]);
		assert_u64_gt(read_u64(name), 0, "the seeded maxima should show");
	}

	expect_d_eq(mallctl("stats.mutexes.reset", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	epoch_advance();

	/*
	 * Only the locks taken since the reset count, and nothing has waited
	 * since: the test is single-threaded.
	 */
	for (unsigned s = 0; s < NSHARDS; s++) {
		for (size_t m = 0; m < ARRAY_COUNT(mutexes); m++) {
			char prefix[160], name[192];
			malloc_snprintf(prefix, sizeof(prefix),
			    "stats.hpa_shard_sets.%u.shards.%u.mutexes.%s",
			    shard_set_ind[s], shard_slot[s], mutexes[m]);
			malloc_snprintf(
			    name, sizeof(name), "%s.max_wait_time", prefix);
			expect_u64_eq(read_u64(name), 0, "%s after the reset",
			    name);
			malloc_snprintf(
			    name, sizeof(name), "%s.max_num_thds", prefix);
			expect_u32_eq(read_u32(name), 0, "%s after the reset",
			    name);
			malloc_snprintf(name, sizeof(name), "%s.num_wait", prefix);
			expect_u64_eq(read_u64(name), 0, "%s after the reset",
			    name);
		}
		char name[160];
		malloc_snprintf(name, sizeof(name),
		    "stats.hpa_shard_sets.%u.shards.%u.mutexes.hpa_shard.num_ops",
		    shard_set_ind[s], shard_slot[s]);
		expect_u64_lt(read_u64(name), ops_before[s],
		    "%s should restart from the reset", name);
	}
}
TEST_END

int
main(void) {
	/*
	 * hpa:true in the .sh would fail jemalloc's initialization where the
	 * HPA is not supported, so turn the HPA on here with guards.
	 */
	if (hpa_supported()) {
		opt_hpa = true;
	}
	return test_no_reentrancy(test_layout, test_shard_leaves,
	    test_set_leaves, test_merged_is_arenas_plus_sets,
	    test_mutexes_reset);
}
