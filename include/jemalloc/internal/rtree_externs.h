#ifndef JEMALLOC_INTERNAL_RTREE_EXTERNS_H
#define JEMALLOC_INTERNAL_RTREE_EXTERNS_H

/*
 * Split the bits into one to three partitions depending on number of
 * significant bits.  It the number of bits does not divide evenly into the
 * number of levels, place one remainder bit per level starting at the leaf
 * level.
 */
static const rtree_level_t rtree_levels[] = {
#if RTREE_HEIGHT == 1
	{RTREE_NSB, RTREE_NHIB + RTREE_NSB}
#elif RTREE_HEIGHT == 2
	{RTREE_NSB/2, RTREE_NHIB + RTREE_NSB/2},
	{RTREE_NSB/2 + RTREE_NSB%2, RTREE_NHIB + RTREE_NSB}
#elif RTREE_HEIGHT == 3
	{RTREE_NSB/3, RTREE_NHIB + RTREE_NSB/3},
	{RTREE_NSB/3 + RTREE_NSB%3/2,
	    RTREE_NHIB + RTREE_NSB/3*2 + RTREE_NSB%3/2},
	{RTREE_NSB/3 + RTREE_NSB%3 - RTREE_NSB%3/2, RTREE_NHIB + RTREE_NSB}
#else
#  error Unsupported rtree height
#endif
};

bool rtree_new(rtree_t *rtree, bool zeroed);

typedef rtree_node_elm_t *(rtree_node_alloc_t)(tsdn_t *, rtree_t *, size_t);
extern rtree_node_alloc_t *JET_MUTABLE rtree_node_alloc;

typedef rtree_leaf_elm_t *(rtree_leaf_alloc_t)(tsdn_t *, rtree_t *, size_t);
extern rtree_leaf_alloc_t *JET_MUTABLE rtree_leaf_alloc;

typedef void (rtree_node_dalloc_t)(tsdn_t *, rtree_t *, rtree_node_elm_t *);
extern rtree_node_dalloc_t *JET_MUTABLE rtree_node_dalloc;

typedef void (rtree_leaf_dalloc_t)(tsdn_t *, rtree_t *, rtree_leaf_elm_t *);
extern rtree_leaf_dalloc_t *JET_MUTABLE rtree_leaf_dalloc;
#ifdef JEMALLOC_JET
void rtree_delete(tsdn_t *tsdn, rtree_t *rtree);
#endif
rtree_leaf_elm_t *rtree_leaf_elm_lookup_hard(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);

#endif /* JEMALLOC_INTERNAL_RTREE_EXTERNS_H */
