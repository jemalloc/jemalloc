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
 *
 * Note that multiple input pointers may reside on a same extent so the output
 * fields may contain duplicates.
 *
 * The format of the input/output looks like:
 *
 * input[0]:  1st_pointer_to_query	|  output[0]: 1st_extent_n_free_regions
 *					|  output[1]: 1st_extent_n_regions
 *					|  output[2]: 1st_extent_size
 * input[1]:  2nd_pointer_to_query	|  output[3]: 2nd_extent_n_free_regions
 *					|  output[4]: 2nd_extent_n_regions
 *					|  output[5]: 2nd_extent_size
 * ...					|  ...
 *
 * The input array and size are respectively passed in by newp and newlen, and
 * the output array and size are respectively oldp and *oldlenp.
 *
 * It can be beneficial to define the following macros to make it easier to
 * access the output:
 *
 * #define NFREE_READ(out, i) out[(i) * 3]
 * #define NREGS_READ(out, i) out[(i) * 3 + 1]
 * #define SIZE_READ(out, i) out[(i) * 3 + 2]
 *
 * and then write e.g. NFREE_READ(oldp, i) to fetch the output.  See the unit
 * test test_batch in test/unit/extent_util.c for a concrete example.
 *
 * A typical workflow would be composed of the following steps:
 *
 * (1) flush tcache: mallctl("thread.tcache.flush", ...)
 * (2) initialize input array of pointers to query fragmentation
 * (3) allocate output array to hold utilization statistics
 * (4) query utilization: mallctl("experimental.utilization.batch_query", ...)
 * (5) (optional) decide if it's worthwhile to defragment; otherwise stop here
 * (6) disable tcache: mallctl("thread.tcache.enabled", ...)
 * (7) defragment allocations with significant fragmentation, e.g.:
 *         for each allocation {
 *             if it's fragmented {
 *                 malloc(...);
 *                 memcpy(...);
 *                 free(...);
 *             }
 *         }
 * (8) enable tcache: mallctl("thread.tcache.enabled", ...)
 *
 * The application can determine the significance of fragmentation themselves
 * relying on the statistics returned, both at the overall level i.e. step "(5)"
 * and at individual allocation level i.e. within step "(7)".  Possible choices
 * are:
 *
 * (a) whether memory utilization ratio is below certain threshold,
 * (b) whether memory consumption is above certain threshold, or
 * (c) some combination of the two.
 *
 * The caller needs to make sure that the input/output arrays are valid and
 * their sizes are proper as well as matched, meaning:
 *
 * (a) newlen = n_pointers * sizeof(const void *)
 * (b) *oldlenp = n_pointers * sizeof(size_t) * 3
 * (c) n_pointers > 0
 *
 * Otherwise, the function immediately returns EINVAL without touching anything.
 *
 * In the rare case where there's no associated extent found for some pointers,
 * rather than immediately terminating the computation and raising an error,
 * the function simply zeros out the corresponding output fields and continues
 * the computation until all input pointers are handled.  The motivations of
 * such a design are as follows:
 *
 * (a) The function always either processes nothing or processes everything, and
 * never leaves the output half touched and half untouched.
 *
 * (b) It facilitates usage needs especially common in C++.  A vast variety of
 * C++ objects are instantiated with multiple dynamic memory allocations.  For
 * example, std::string and std::vector typically use at least two allocations,
 * one for the metadata and one for the actual content.  Other types may use
 * even more allocations.  When inquiring about utilization statistics, the
 * caller often wants to examine into all such allocations, especially internal
 * one(s), rather than just the topmost one.  The issue comes when some
 * implementations do certain optimizations to reduce/aggregate some internal
 * allocations, e.g. putting short strings directly into the metadata, and such
 * decisions are not known to the caller.  Therefore, we permit pointers to
 * memory usages that may not be returned by previous malloc calls, and we
 * provide the caller a convenient way to identify such cases.
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
