#ifndef JEMALLOC_INTERNAL_PSSET_H
#define JEMALLOC_INTERNAL_PSSET_H

/*
 * A page-slab set.  What the eset is to PAC, the psset is to HPA.  It maintains
 * a collection of page-slabs (the intent being that they are backed by
 * hugepages, or at least could be), and handles allocation and deallocation
 * requests.
 *
 * It has the same synchronization guarantees as the eset; stats queries don't
 * need any external synchronization, everything else does.
 */

/*
 * One more than the maximum pszind_t we will serve out of the HPA.
 * Practically, we expect only the first few to be actually used.  This
 * corresponds to a maximum size of of 512MB on systems with 4k pages and
 * SC_NGROUP == 4, which is already an unreasonably large maximum.  Morally, you
 * can think of this as being SC_NPSIZES, but there's no sense in wasting that
 * much space in the arena, making bitmaps that much larger, etc.
 */
#define PSSET_NPSIZES 64

typedef struct psset_bin_stats_s psset_bin_stats_t;
struct psset_bin_stats_s {
	/* How many pageslabs are in this bin? */
	size_t npageslabs;
	/* Of them, how many pages are active? */
	size_t nactive;
	/* How many are inactive? */
	size_t ninactive;
};

static inline void
psset_bin_stats_accum(psset_bin_stats_t *dst, psset_bin_stats_t *src) {
	dst->npageslabs += src->npageslabs;
	dst->nactive += src->nactive;
	dst->ninactive += src->ninactive;
}

typedef struct psset_s psset_t;
struct psset_s {
	/*
	 * The pageslabs, quantized by the size class of the largest contiguous
	 * free run of pages in a pageslab.
	 */
	edata_age_heap_t pageslabs[PSSET_NPSIZES];
	bitmap_t bitmap[BITMAP_GROUPS(PSSET_NPSIZES)];
	/*
	 * Full slabs don't live in any edata heap.  But we still track their
	 * stats.
	 */
	psset_bin_stats_t full_slab_stats;
	psset_bin_stats_t slab_stats[PSSET_NPSIZES];

	/* How many alloc_new calls have happened? */
	uint64_t age_counter;
};

void psset_init(psset_t *psset);

/*
 * Tries to obtain a chunk from an existing pageslab already in the set.
 * Returns true on failure.
 */
bool psset_alloc_reuse(psset_t *psset, edata_t *r_edata, size_t size);

/*
 * Given a newly created pageslab ps (not currently in the set), pass ownership
 * to the psset and allocate an extent from within it.  The passed-in pageslab
 * must be at least as big as size.
 */
void psset_alloc_new(psset_t *psset, edata_t *ps,
    edata_t *r_edata, size_t size);

/*
 * Given an extent that comes from a pageslab in this pageslab set, returns it
 * to its slab.  Does not take ownership of the underlying edata_t.
 *
 * If some slab becomes empty as a result of the dalloc, it is retuend -- the
 * result must be checked and deallocated to the central HPA.  Otherwise returns
 * NULL.
 */
edata_t *psset_dalloc(psset_t *psset, edata_t *edata);

#endif /* JEMALLOC_INTERNAL_PSSET_H */
