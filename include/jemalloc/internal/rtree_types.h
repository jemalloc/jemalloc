#ifndef JEMALLOC_INTERNAL_RTREE_TYPES_H
#define JEMALLOC_INTERNAL_RTREE_TYPES_H

/*
 * This radix tree implementation is tailored to the singular purpose of
 * associating metadata with extents that are currently owned by jemalloc.
 *
 *******************************************************************************
 */

typedef struct rtree_elm_s rtree_elm_t;
typedef struct rtree_elm_witness_s rtree_elm_witness_t;
typedef struct rtree_elm_witness_tsd_s rtree_elm_witness_tsd_t;
typedef struct rtree_level_s rtree_level_t;
typedef struct rtree_ctx_s rtree_ctx_t;
typedef struct rtree_s rtree_t;

/*
 * RTREE_BITS_PER_LEVEL must be a power of two that is no larger than the
 * machine address width.
 */
#define LG_RTREE_BITS_PER_LEVEL	4
#define RTREE_BITS_PER_LEVEL	(1U << LG_RTREE_BITS_PER_LEVEL)
/* Maximum rtree height. */
#define RTREE_HEIGHT_MAX						\
    ((1U << (LG_SIZEOF_PTR+3)) / RTREE_BITS_PER_LEVEL)

#define RTREE_CTX_INITIALIZER	{					\
	false,								\
	0,								\
	0,								\
	{NULL /* C initializes all trailing elements to NULL. */}	\
}

/*
 * Maximum number of concurrently acquired elements per thread.  This controls
 * how many witness_t structures are embedded in tsd.  Ideally rtree_elm_t would
 * have a witness_t directly embedded, but that would dramatically bloat the
 * tree.  This must contain enough entries to e.g. coalesce two extents.
 */
#define RTREE_ELM_ACQUIRE_MAX	4

/* Initializers for rtree_elm_witness_tsd_t. */
#define RTREE_ELM_WITNESS_INITIALIZER {					\
	NULL,								\
	WITNESS_INITIALIZER("rtree_elm", WITNESS_RANK_RTREE_ELM)	\
}

#define RTREE_ELM_WITNESS_TSD_INITIALIZER {				\
	{								\
		RTREE_ELM_WITNESS_INITIALIZER,				\
		RTREE_ELM_WITNESS_INITIALIZER,				\
		RTREE_ELM_WITNESS_INITIALIZER,				\
		RTREE_ELM_WITNESS_INITIALIZER				\
	}								\
}

#endif /* JEMALLOC_INTERNAL_RTREE_TYPES_H */
