#ifndef JEMALLOC_INTERNAL_PSSET_H
#define JEMALLOC_INTERNAL_PSSET_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/hpdata.h"
#include "jemalloc/internal/sc.h"

/*
 * A page-slab set.  What the eset is to PAC, the psset is to HPA.  It maintains
 * a collection of page-slabs (the intent being that they are backed by
 * hugepages, or at least could be), and handles allocation and deallocation
 * requests.
 */

/* Number of page size classes tracked for statistics and purging. */
#define PSSET_NPSIZES 64

/*
 * Allocation heaps only need page size classes through HUGEPAGE.  Use the
 * minimum page size for dynamic builds, and retain the existing upper limit.
 */
#define PSSET_LG_HUGEPAGE_PAGES (LG_HUGEPAGE - LG_PAGE_OR_MIN)
#if PSSET_LG_HUGEPAGE_PAGES < SC_LG_NGROUP
#define PSSET_HUGEPAGE_NPSIZES (1U << PSSET_LG_HUGEPAGE_PAGES)
#else
#define PSSET_HUGEPAGE_NPSIZES \
    (SC_NGROUP * (PSSET_LG_HUGEPAGE_PAGES - SC_LG_NGROUP + 1))
#endif
#if PSSET_HUGEPAGE_NPSIZES < PSSET_NPSIZES
#define PSSET_NALLOC_BINS PSSET_HUGEPAGE_NPSIZES
#else
#define PSSET_NALLOC_BINS PSSET_NPSIZES
#endif

/*
 * We store non-hugefied and hugified pageslabs metadata separately.
 * [0] corresponds to non-hugified and [1] to hugified pageslabs.
 */
#define PSSET_NHUGE 2

/*
 * We keep two purge lists per page size class; one for hugified hpdatas (at
 * index 2*pszind), and one for the non-hugified hpdatas (at index 2*pszind +
 * 1).  This lets us implement a preference for purging non-hugified hpdatas
 * among similarly-dirty ones.
 * We reserve the last two indices for empty slabs, in that case purging
 * hugified ones (which are definitionally all waste) before non-hugified ones
 * (i.e. reversing the order).
 */
#define PSSET_NPURGE_LISTS (2 * PSSET_NPSIZES)

typedef struct psset_bin_stats_s psset_bin_stats_t;
struct psset_bin_stats_s {
	/* How many pageslabs are in this bin? */
	size_t npageslabs;
	/* Of them, how many pages are active? */
	size_t nactive;
	/* And how many are dirty? */
	size_t ndirty;
};

typedef struct psset_stats_s psset_stats_t;
struct psset_stats_s {
	/*
	 * Merged stats for all pageslabs in psset.  This lets us quickly
	 * answer queries for the number of dirty and active pages in the
	 * entire set.
	 */
	psset_bin_stats_t merged;

	/*
	 * Below are the same stats, but aggregated by different
	 * properties of pageslabs: huginess or fullness.
	 */

	/* Non-huge and huge slabs. */
	psset_bin_stats_t slabs[PSSET_NHUGE];

	/*
	 * The second index is huge stats; nonfull_slabs[pszind][0] contains
	 * stats for the non-huge slabs in bucket pszind, while
	 * nonfull_slabs[pszind][1] contains stats for the huge slabs.
	 */
	psset_bin_stats_t nonfull_slabs[PSSET_NPSIZES][PSSET_NHUGE];

	/*
	 * Full slabs don't live in any edata heap, but we still track their
	 * stats.
	 */
	psset_bin_stats_t full_slabs[PSSET_NHUGE];

	/* Empty slabs are similar. */
	psset_bin_stats_t empty_slabs[PSSET_NHUGE];
};

typedef struct psset_s psset_t;
struct psset_s {
	/*
	 * The pageslabs, quantized by the size class of the largest contiguous
	 * free run of pages in a pageslab.
	 */
	hpdata_age_heap_t pageslabs[PSSET_NALLOC_BINS];
	/* Bitmap for which set bits correspond to non-empty heaps. */
	fb_group_t    pageslab_bitmap[FB_NGROUPS(PSSET_NALLOC_BINS)];
	psset_stats_t stats;
	/*
	 * Slabs with no active allocations, but which are allowed to serve new
	 * allocations.
	 */
	hpdata_empty_list_t empty;
	/*
	 * Slabs which are available to be purged, ordered by how much we want
	 * to purge them (with later indices indicating slabs we want to purge
	 * more).
	 */
	hpdata_purge_list_t to_purge[PSSET_NPURGE_LISTS];
	/* Bitmap for which set bits correspond to non-empty purge lists. */
	fb_group_t purge_bitmap[FB_NGROUPS(PSSET_NPURGE_LISTS)];
	/* Slabs which are available to be hugified. */
	hpdata_hugify_list_t to_hugify;
};

void psset_init(psset_t *psset);
void psset_stats_accum(psset_stats_t *dst, psset_stats_t *src);

/*
 * Begin or end updating the given pageslab's metadata.  While the pageslab is
 * being updated, it won't be returned from psset_fit calls.
 */
void psset_update_begin(psset_t *psset, hpdata_t *ps);
void psset_update_end(psset_t *psset, hpdata_t *ps);

/* Analogous to the eset_fit; pick a hpdata to serve the request. */
hpdata_t *psset_pick_alloc(psset_t *psset, size_t size);
/* Pick the pageslab to purge, or NULL if none is available for purging. */
hpdata_t *psset_pick_purge(psset_t *psset);

/* Pick one to hugify. */
hpdata_t *psset_pick_hugify(psset_t *psset);

void psset_insert(psset_t *psset, hpdata_t *ps);
void psset_remove(psset_t *psset, hpdata_t *ps);

static inline size_t
psset_npageslabs(const psset_t *psset) {
	return psset->stats.merged.npageslabs;
}

static inline size_t
psset_nactive(const psset_t *psset) {
	return psset->stats.merged.nactive;
}

static inline size_t
psset_ndirty(const psset_t *psset) {
	return psset->stats.merged.ndirty;
}

#endif /* JEMALLOC_INTERNAL_PSSET_H */
