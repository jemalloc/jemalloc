#ifndef JEMALLOC_INTERNAL_RTREE_TYPES_H
#define JEMALLOC_INTERNAL_RTREE_TYPES_H

#include "jemalloc/internal/size_classes.h"

/*
 * This radix tree implementation is tailored to the singular purpose of
 * associating metadata with extents that are currently owned by jemalloc.
 *
 *******************************************************************************
 */

typedef struct rtree_node_elm_s rtree_node_elm_t;
typedef struct rtree_leaf_elm_s rtree_leaf_elm_t;
typedef struct rtree_level_s rtree_level_t;
typedef struct rtree_s rtree_t;

/* Number of high insignificant bits. */
#define RTREE_NHIB ((1U << (LG_SIZEOF_PTR+3)) - LG_VADDR)
/* Number of low insigificant bits. */
#define RTREE_NLIB LG_PAGE
/* Number of significant bits. */
#define RTREE_NSB (LG_VADDR - RTREE_NLIB)
/* Number of levels in radix tree. */
#if RTREE_NSB <= 10
#  define RTREE_HEIGHT 1
#elif RTREE_NSB <= 36
#  define RTREE_HEIGHT 2
#elif RTREE_NSB <= 52
#  define RTREE_HEIGHT 3
#else
#  error Unsupported number of significant virtual address bits
#endif
/* Use compact leaf representation if virtual address encoding allows. */
#if RTREE_NHIB >= LG_CEIL_NSIZES
#  define RTREE_LEAF_COMPACT
#endif

/* Needed for initialization only. */
#define RTREE_LEAFKEY_INVALID ((uintptr_t)1)

/*
 * Number of leafkey/leaf pairs to cache in L1 and L2 level respectively.  Each
 * entry supports an entire leaf, so the cache hit rate is typically high even
 * with a small number of entries.  In rare cases extent activity will straddle
 * the boundary between two leaf nodes.  Furthermore, an arena may use a
 * combination of dss and mmap.  Note that as memory usage grows past the amount
 * that this cache can directly cover, the cache will become less effective if
 * locality of reference is low, but the consequence is merely cache misses
 * while traversing the tree nodes.
 *
 * The L1 direct mapped cache offers consistent and low cost on cache hit.
 * However collision could affect hit rate negatively.  This is resolved by
 * combining with a L2 LRU cache, which requires linear search and re-ordering
 * on access but suffers no collision.  Note that, the cache will itself suffer
 * cache misses if made overly large, plus the cost of linear search in the LRU
 * cache.
 */
#define RTREE_CTX_LG_NCACHE 4
#define RTREE_CTX_NCACHE (1 << RTREE_CTX_LG_NCACHE)
#define RTREE_CTX_NCACHE_L2 8

/*
 * Zero initializer required for tsd initialization only.  Proper initialization
 * done via rtree_ctx_data_init().
 */
#define RTREE_CTX_ZERO_INITIALIZER {{{0}}}

#endif /* JEMALLOC_INTERNAL_RTREE_TYPES_H */
