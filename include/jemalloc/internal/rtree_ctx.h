#ifndef JEMALLOC_INTERNAL_RTREE_CTX_H
#define JEMALLOC_INTERNAL_RTREE_CTX_H

#include "jemalloc/internal/rtree_types.h"

typedef struct rtree_ctx_cache_elm_s rtree_ctx_cache_elm_t;
struct rtree_ctx_cache_elm_s {
	uintptr_t		leafkey;
	rtree_leaf_elm_t	*leaf;
};

typedef struct rtree_ctx_s rtree_ctx_t;
struct rtree_ctx_s {
	/* Direct mapped cache. */
	rtree_ctx_cache_elm_t	cache[RTREE_CTX_NCACHE];
	/* L2 LRU cache. */
	rtree_ctx_cache_elm_t	l2_cache[RTREE_CTX_NCACHE_L2];
};

void rtree_ctx_data_init(rtree_ctx_t *ctx);

#endif /* JEMALLOC_INTERNAL_RTREE_CTX_H */
