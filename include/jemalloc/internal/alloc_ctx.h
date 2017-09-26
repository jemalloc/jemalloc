#ifndef JEMALLOC_INTERNAL_ALLOC_CTX_H
#define JEMALLOC_INTERNAL_ALLOC_CTX_H

#include "jemalloc/internal/jemalloc_internal_types.h"

/*
 * Summarizes the key metadata we've looked up for an allocation -- its size and
 * whether it's a slab.  This can be used to avoid unnecessary rtree lookups.
 */
typedef struct alloc_ctx_s alloc_ctx_t;
struct alloc_ctx_s {
	szind_t szind;
	bool slab;
};

#endif /* JEMALLOC_INTERNAL_ALLOC_CTX_H */
