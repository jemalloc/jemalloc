#ifndef JEMALLOC_INTERNAL_EXTENT_STRUCTS_H
#define JEMALLOC_INTERNAL_EXTENT_STRUCTS_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/slab_data.h"

/*
 * The following two structs are for experimental purposes. See
 * experimental_utilization_query_ctl and
 * experimental_utilization_batch_query_ctl in src/ctl.c.
 */

struct extent_util_stats_s {
	size_t nfree;
	size_t nregs;
	size_t size;
};

struct extent_util_stats_verbose_s {
	void *slabcur_addr;
	size_t nfree;
	size_t nregs;
	size_t size;
	size_t bin_nfree;
	size_t bin_nregs;
};

#endif /* JEMALLOC_INTERNAL_EXTENT_STRUCTS_H */
