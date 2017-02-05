#ifndef JEMALLOC_INTERNAL_RTREE_STRUCTS_H
#define JEMALLOC_INTERNAL_RTREE_STRUCTS_H

struct rtree_elm_s {
	union {
		void		*pun;
		rtree_elm_t	*child;
		extent_t	*extent;
	};
};

struct rtree_elm_witness_s {
	const rtree_elm_t	*elm;
	witness_t		witness;
};

struct rtree_elm_witness_tsd_s {
	rtree_elm_witness_t	witnesses[RTREE_ELM_ACQUIRE_MAX];
};

struct rtree_level_s {
	/* Number of key bits distinguished by this level. */
	unsigned		bits;
	/*
	 * Cumulative number of key bits distinguished by traversing to
	 * corresponding tree level.
	 */
	unsigned		cumbits;
};

struct rtree_ctx_cache_elm_s {
	uintptr_t	key;
	rtree_elm_t	*elm;
};

struct rtree_ctx_s {
	rtree_ctx_cache_elm_t cache[RTREE_CTX_NCACHE];
};

struct rtree_s {
	unsigned		height;
	union {
		void		*root_pun;
		rtree_elm_t	*root;
	};
	rtree_level_t		levels[RTREE_HEIGHT_MAX];
	malloc_mutex_t		init_lock;
};

#endif /* JEMALLOC_INTERNAL_RTREE_STRUCTS_H */
