#ifndef JEMALLOC_INTERNAL_RTREE_WITNESS_H
#define JEMALLOC_INTERNAL_RTREE_WITNESS_H

#include "jemalloc/internal/rtree_types.h"
#include "jemalloc/internal/witness_types.h"
#include "jemalloc/internal/witness_structs.h"

typedef struct rtree_leaf_elm_witness_s rtree_leaf_elm_witness_t;
struct rtree_leaf_elm_witness_s {
	const rtree_leaf_elm_t	*elm;
	witness_t		witness;
};

typedef struct rtree_leaf_elm_witness_tsd_s rtree_leaf_elm_witness_tsd_t;
struct rtree_leaf_elm_witness_tsd_s {
	rtree_leaf_elm_witness_t	witnesses[RTREE_ELM_ACQUIRE_MAX];
};

#endif /* JEMALLOC_INTERNAL_RTREE_WITNESS_H */
