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
	/*
	 * A non-NULL subtree points to a subtree rooted along the hypothetical
	 * path to the leaf node corresponding to key 0.  Depending on what keys
	 * have been used to store to the tree, an arbitrary combination of
	 * subtree pointers may remain NULL.
	 *
	 * Suppose keys comprise 48 bits, and LG_RTREE_BITS_PER_LEVEL is 4.
	 * This results in a 3-level tree, and the leftmost leaf can be directly
	 * accessed via levels[2], the subtree prefixed by 0x0000 (excluding
	 * 0x00000000) can be accessed via levels[1], and the remainder of the
	 * tree can be accessed via levels[0].
	 *
	 *   levels[0] : [<unused> | 0x0001******** | 0x0002******** | ...]
	 *
	 *   levels[1] : [<unused> | 0x00000001**** | 0x00000002**** | ... ]
	 *
	 *   levels[2] : [extent(0x000000000000) | extent(0x000000000001) | ...]
	 *
	 * This has practical implications on x64, which currently uses only the
	 * lower 47 bits of virtual address space in userland, thus leaving
	 * levels[0] unused and avoiding a level of tree traversal.
	 */
	union {
		void		*subtree_pun;
		rtree_elm_t	*subtree;
	};
	/* Number of key bits distinguished by this level. */
	unsigned		bits;
	/*
	 * Cumulative number of key bits distinguished by traversing to
	 * corresponding tree level.
	 */
	unsigned		cumbits;
};

struct rtree_ctx_s {
	/* If false, key/elms have not yet been initialized by a lookup. */
	bool		valid;
	/* Key that corresponds to the tree path recorded in elms. */
	uintptr_t	key;
	/* Memoized rtree_start_level(key). */
	unsigned	start_level;
	/*
	 * A path through rtree, driven by key.  Only elements that could
	 * actually be used for subsequent lookups are initialized, i.e. if
	 * start_level = rtree_start_level(key) is non-zero, the first
	 * start_level elements are uninitialized.  The last element contains a
	 * pointer to the leaf node element that corresponds to key, so that
	 * exact matches require no tree node offset computation.
	 */
	rtree_elm_t	*elms[RTREE_HEIGHT_MAX + 1];
};

struct rtree_s {
	unsigned		height;
	/*
	 * Precomputed table used to convert from the number of leading 0 key
	 * bits to which subtree level to start at.
	 */
	unsigned		start_level[RTREE_HEIGHT_MAX + 1];
	rtree_level_t		levels[RTREE_HEIGHT_MAX];
	malloc_mutex_t		init_lock;
};

#endif /* JEMALLOC_INTERNAL_RTREE_STRUCTS_H */
