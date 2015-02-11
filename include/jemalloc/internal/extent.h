/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct extent_node_s extent_node_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* Tree of extents. */
struct extent_node_s {
	/* Arena from which this extent came, if any. */
	arena_t			*arena;

	/* Pointer to the extent that this tree node is responsible for. */
	void			*addr;

	/*
	 * Total region size, or 0 if this node corresponds to an arena chunk.
	 */
	size_t			size;

	/*
	 * 'prof_tctx' and 'zeroed' are never needed at the same time, so
	 * overlay them in order to fit extent_node_t in one cache line.
	 */
	union {
		/* Profile counters, used for huge objects. */
		prof_tctx_t	*prof_tctx;

		/* True if zero-filled; used by chunk recycling code. */
		bool		zeroed;
	};

	union {
		/* Linkage for the size/address-ordered tree. */
		rb_node(extent_node_t)	link_szad;

		/* Linkage for huge allocations and cached chunks nodes. */
		ql_elm(extent_node_t)	link_ql;
	};

	/* Linkage for the address-ordered tree. */
	rb_node(extent_node_t)	link_ad;
};
typedef rb_tree(extent_node_t) extent_tree_t;

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

rb_proto(, extent_tree_szad_, extent_tree_t, extent_node_t)

rb_proto(, extent_tree_ad_, extent_tree_t, extent_node_t)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

