#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/inspect.h"

/******************************************************************************/
/* experimental.utilization.* mallctl handlers. */

/*
 * Given an input array of pointers, output three memory utilization entries of
 * type size_t for each input pointer about the extent it resides in:
 *
 * (a) number of free regions in the extent,
 * (b) number of regions in the extent, and
 * (c) size of the extent in terms of bytes.
 *
 * This API is mainly intended for small class allocations, where extents are
 * used as slab.  In case of large class allocations, the outputs are trivial:
 * "(a)" will be 0, "(b)" will be 1, and "(c)" will be the usable size.
 */
int
experimental_utilization_batch_query_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret;

	assert(sizeof(inspect_extent_util_stats_t) == sizeof(size_t) * 3);

	const size_t len = newlen / sizeof(const void *);
	if (oldp == NULL || oldlenp == NULL || newp == NULL || newlen == 0
	    || newlen != len * sizeof(const void *)
	    || *oldlenp != len * sizeof(inspect_extent_util_stats_t)) {
		ret = EINVAL;
		goto label_return;
	}

	void                        **ptrs = (void **)newp;
	inspect_extent_util_stats_t *util_stats =
	    (inspect_extent_util_stats_t *)oldp;
	size_t i;
	for (i = 0; i < len; ++i) {
		inspect_extent_util_stats_get(tsd_tsdn(tsd), ptrs[i],
		    &util_stats[i].nfree, &util_stats[i].nregs,
		    &util_stats[i].size);
	}
	ret = 0;

label_return:
	return ret;
}
