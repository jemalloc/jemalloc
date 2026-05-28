#ifndef JEMALLOC_INTERNAL_INSPECT_H
#define JEMALLOC_INTERNAL_INSPECT_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/tsd_types.h"

/*
 * This module contains the heap introspection capabilities.  For now they are
 * exposed purely through mallctl APIs in the experimental namespace, but this
 * may change over time.
 */

/*
 * The following struct is for experimental purposes. See
 * experimental_utilization_batch_query_ctl in src/ctl.c.
 */
typedef struct inspect_extent_util_stats_s inspect_extent_util_stats_t;
struct inspect_extent_util_stats_s {
	size_t nfree;
	size_t nregs;
	size_t size;
};

void inspect_extent_util_stats_get(
    tsdn_t *tsdn, const void *ptr, size_t *nfree, size_t *nregs, size_t *size);

#endif /* JEMALLOC_INTERNAL_INSPECT_H */
