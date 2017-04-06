#ifndef JEMALLOC_INTERNAL_TCACHE_STRUCTS_H
#define JEMALLOC_INTERNAL_TCACHE_STRUCTS_H

/*
 * Read-only information associated with each element of tcache_t's tbins array
 * is stored separately, mainly to reduce memory usage.
 */
struct tcache_bin_info_s {
	unsigned	ncached_max;	/* Upper limit on ncached. */
};

struct tcache_bin_s {
	low_water_t	low_water;	/* Min # cached since last GC. */
	uint32_t	ncached;	/* # of cached objects. */
	/*
	 * ncached and stats are both modified frequently.  Let's keep them
	 * close so that they have a higher chance of being on the same
	 * cacheline, thus less write-backs.
	 */
	tcache_bin_stats_t tstats;
	/*
	 * To make use of adjacent cacheline prefetch, the items in the avail
	 * stack goes to higher address for newer allocations.  avail points
	 * just above the available space, which means that
	 * avail[-ncached, ... -1] are available items and the lowest item will
	 * be allocated first.
	 */
	void		**avail;	/* Stack of available objects. */
};

struct tcache_s {
	/* Data accessed frequently first: prof, ticker and small bins. */
	uint64_t	prof_accumbytes;/* Cleared after arena_prof_accum(). */
	ticker_t	gc_ticker;	/* Drives incremental GC. */
	/*
	 * The pointer stacks associated with tbins follow as a contiguous
	 * array.  During tcache initialization, the avail pointer in each
	 * element of tbins is initialized to point to the proper offset within
	 * this array.
	 */
#ifdef JEMALLOC_TCACHE
	tcache_bin_t	tbins_small[NBINS];
#else
	tcache_bin_t	tbins_small[0];
#endif
	/* Data accessed less often below. */
	ql_elm(tcache_t) link;		/* Used for aggregating stats. */
	arena_t		*arena;		/* Associated arena. */
	szind_t		next_gc_bin;	/* Next bin to GC. */
#ifdef JEMALLOC_TCACHE
	/* For small bins, fill (ncached_max >> lg_fill_div). */
	uint8_t		lg_fill_div[NBINS];
	tcache_bin_t	tbins_large[NSIZES-NBINS];
#else
	uint8_t		lg_fill_div[0];
	tcache_bin_t	tbins_large[0];
#endif
};

/* Linkage for list of available (previously used) explicit tcache IDs. */
struct tcaches_s {
	union {
		tcache_t	*tcache;
		tcaches_t	*next;
	};
};

#endif /* JEMALLOC_INTERNAL_TCACHE_STRUCTS_H */
