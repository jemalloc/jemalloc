#ifndef JEMALLOC_INTERNAL_TCACHE_STRUCTS_H
#define JEMALLOC_INTERNAL_TCACHE_STRUCTS_H

#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/size_classes.h"
#include "jemalloc/internal/cache_bin.h"
#include "jemalloc/internal/ticker.h"

struct tcache_s {
	/* Data accessed frequently first: prof, ticker and small bins. */
	uint64_t	prof_accumbytes;/* Cleared after arena_prof_accum(). */
	ticker_t	gc_ticker;	/* Drives incremental GC. */
	/*
	 * The pointer stacks associated with bins follow as a contiguous array.
	 * During tcache initialization, the avail pointer in each element of
	 * tbins is initialized to point to the proper offset within this array.
	 */
	cache_bin_t	bins_small[NBINS];
	/* Data accessed less often below. */
	ql_elm(tcache_t) link;		/* Used for aggregating stats. */
	arena_t		*arena;		/* Associated arena. */
	szind_t		next_gc_bin;	/* Next bin to GC. */
	/* For small bins, fill (ncached_max >> lg_fill_div). */
	uint8_t		lg_fill_div[NBINS];
	cache_bin_t	bins_large[NSIZES-NBINS];
};

/* Linkage for list of available (previously used) explicit tcache IDs. */
struct tcaches_s {
	union {
		tcache_t	*tcache;
		tcaches_t	*next;
	};
};

#endif /* JEMALLOC_INTERNAL_TCACHE_STRUCTS_H */
