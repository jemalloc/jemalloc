#ifndef JEMALLOC_INTERNAL_PEAK_DEMAND_H
#define JEMALLOC_INTERNAL_PEAK_DEMAND_H

#include "jemalloc/internal/jemalloc_preamble.h"

/*
 * Implementation of peak active memory demand tracking.
 *
 * Inspired by "Beyond malloc efficiency to fleet efficiency: a hugepage-aware
 * memory allocator" whitepaper.
 * https://storage.googleapis.com/gweb-research2023-media/pubtools/6170.pdf
 *
 * End goal is to track peak active memory usage over specified time interval.
 * We do so by dividing this time interval into disjoint subintervals and
 * storing value of maximum memory usage for each subinterval in a circular
 * buffer.  Nanoseconds resolution timestamp uniquely maps into epoch, which is
 * used as an index to access circular buffer.
 */

#define PEAK_DEMAND_LG_BUCKETS 4
/*
 * Number of buckets should be power of 2 to ensure modulo operation is
 * optimized to bit masking by the compiler.
 */
#define PEAK_DEMAND_NBUCKETS (1 << PEAK_DEMAND_LG_BUCKETS)

typedef struct peak_demand_s peak_demand_t;
struct peak_demand_s {
	/*
	 * Absolute value of current epoch, monotonically increases over time.  Epoch
	 * value modulo number of buckets used as an index to access nactive_max
	 * array.
	 */
	uint64_t epoch;

	/* How many nanoseconds each epoch approximately takes. */
	uint64_t epoch_interval_ns;

	/*
	 * Circular buffer to track maximum number of active pages for each
	 * epoch.
	 */
	size_t nactive_max[PEAK_DEMAND_NBUCKETS];
};

void peak_demand_init(peak_demand_t *peak_demand, uint64_t interval_ms);

/* Updates peak demand statistics with current number of active pages. */
void peak_demand_update(peak_demand_t *peak_demand, const nstime_t *now,
    size_t nactive);

/* Returns maximum number of active pages in sliding window. */
size_t peak_demand_nactive_max(peak_demand_t *peak_demand);

#endif /* JEMALLOC_INTERNAL_PEAK_DEMAND_H */
