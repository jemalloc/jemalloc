#ifndef JEMALLOC_INTERNAL_TSD_BINSHARDS_H
#define JEMALLOC_INTERNAL_TSD_BINSHARDS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/sc.h"

/*
 * Per-thread cache of bin-shard assignments.  This lives in its own header
 * (separate from bin.h) so that tsd_internals.h can pull it in for X-macro
 * expansion without dragging in mutex.h, which itself depends on TSD machinery
 * and would form an include-order dependency cycle.
 */

#define TSD_BINSHARDS_ZERO_INITIALIZER                                         \
	{                                                                      \
		{ UINT8_MAX }                                                  \
	}

typedef struct tsd_binshards_s tsd_binshards_t;
struct tsd_binshards_s {
	uint8_t binshard[SC_NBINS];
};

#endif /* JEMALLOC_INTERNAL_TSD_BINSHARDS_H */
