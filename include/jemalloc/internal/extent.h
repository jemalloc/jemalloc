/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct extent_s extent_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* Extent (span of pages).  Use accessor functions for e_* fields. */
struct extent_s {
	/* Arena from which this extent came, if any. */
	arena_t			*e_arena;

	/* Pointer to the extent that this structure is responsible for. */
	void			*e_addr;

	/* Total region size. */
	size_t			e_size;

	/*
	 * The zeroed flag is used by chunk recycling code to track whether
	 * memory is zero-filled.
	 */
	bool			e_zeroed;

	/*
	 * True if physical memory is committed to the extent, whether
	 * explicitly or implicitly as on a system that overcommits and
	 * satisfies physical memory needs on demand via soft page faults.
	 */
	bool			e_committed;

	/*
	 * The achunk flag is used to validate that huge allocation lookups
	 * don't return arena chunks.
	 */
	bool			e_achunk;

	/* Profile counters, used for huge objects. */
	prof_tctx_t		*e_prof_tctx;

	/* Linkage for arena's runs_dirty and chunks_cache rings. */
	arena_runs_dirty_link_t	rd;
	qr(extent_t)		cc_link;

	union {
		/* Linkage for the size/address-ordered tree. */
		rb_node(extent_t)	szad_link;

		/* Linkage for arena's achunks, huge, and node_cache lists. */
		ql_elm(extent_t)	ql_link;
	};

	/* Linkage for the address-ordered tree. */
	rb_node(extent_t)	ad_link;
};
typedef rb_tree(extent_t) extent_tree_t;

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

rb_proto(, extent_tree_szad_, extent_tree_t, extent_t)

rb_proto(, extent_tree_ad_, extent_tree_t, extent_t)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
arena_t	*extent_arena_get(const extent_t *extent);
void	*extent_addr_get(const extent_t *extent);
size_t	extent_size_get(const extent_t *extent);
bool	extent_zeroed_get(const extent_t *extent);
bool	extent_committed_get(const extent_t *extent);
bool	extent_achunk_get(const extent_t *extent);
prof_tctx_t	*extent_prof_tctx_get(const extent_t *extent);
void	extent_arena_set(extent_t *extent, arena_t *arena);
void	extent_addr_set(extent_t *extent, void *addr);
void	extent_size_set(extent_t *extent, size_t size);
void	extent_zeroed_set(extent_t *extent, bool zeroed);
void	extent_committed_set(extent_t *extent, bool committed);
void	extent_achunk_set(extent_t *extent, bool achunk);
void	extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx);
void	extent_init(extent_t *extent, arena_t *arena, void *addr,
    size_t size, bool zeroed, bool committed);
void	extent_dirty_linkage_init(extent_t *extent);
void	extent_dirty_insert(extent_t *extent,
    arena_runs_dirty_link_t *runs_dirty, extent_t *chunks_dirty);
void	extent_dirty_remove(extent_t *extent);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_EXTENT_C_))
JEMALLOC_INLINE arena_t *
extent_arena_get(const extent_t *extent)
{

	return (extent->e_arena);
}

JEMALLOC_INLINE void *
extent_addr_get(const extent_t *extent)
{

	return (extent->e_addr);
}

JEMALLOC_INLINE size_t
extent_size_get(const extent_t *extent)
{

	return (extent->e_size);
}

JEMALLOC_INLINE bool
extent_zeroed_get(const extent_t *extent)
{

	return (extent->e_zeroed);
}

JEMALLOC_INLINE bool
extent_committed_get(const extent_t *extent)
{

	assert(!extent->e_achunk);
	return (extent->e_committed);
}

JEMALLOC_INLINE bool
extent_achunk_get(const extent_t *extent)
{

	return (extent->e_achunk);
}

JEMALLOC_INLINE prof_tctx_t *
extent_prof_tctx_get(const extent_t *extent)
{

	return (extent->e_prof_tctx);
}

JEMALLOC_INLINE void
extent_arena_set(extent_t *extent, arena_t *arena)
{

	extent->e_arena = arena;
}

JEMALLOC_INLINE void
extent_addr_set(extent_t *extent, void *addr)
{

	extent->e_addr = addr;
}

JEMALLOC_INLINE void
extent_size_set(extent_t *extent, size_t size)
{

	extent->e_size = size;
}

JEMALLOC_INLINE void
extent_zeroed_set(extent_t *extent, bool zeroed)
{

	extent->e_zeroed = zeroed;
}

JEMALLOC_INLINE void
extent_committed_set(extent_t *extent, bool committed)
{

	extent->e_committed = committed;
}

JEMALLOC_INLINE void
extent_achunk_set(extent_t *extent, bool achunk)
{

	extent->e_achunk = achunk;
}

JEMALLOC_INLINE void
extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx)
{

	extent->e_prof_tctx = tctx;
}

JEMALLOC_INLINE void
extent_init(extent_t *extent, arena_t *arena, void *addr, size_t size,
    bool zeroed, bool committed)
{

	extent_arena_set(extent, arena);
	extent_addr_set(extent, addr);
	extent_size_set(extent, size);
	extent_zeroed_set(extent, zeroed);
	extent_committed_set(extent, committed);
	extent_achunk_set(extent, false);
	if (config_prof)
		extent_prof_tctx_set(extent, NULL);
}

JEMALLOC_INLINE void
extent_dirty_linkage_init(extent_t *extent)
{

	qr_new(&extent->rd, rd_link);
	qr_new(extent, cc_link);
}

JEMALLOC_INLINE void
extent_dirty_insert(extent_t *extent,
    arena_runs_dirty_link_t *runs_dirty, extent_t *chunks_dirty)
{

	qr_meld(runs_dirty, &extent->rd, rd_link);
	qr_meld(chunks_dirty, extent, cc_link);
}

JEMALLOC_INLINE void
extent_dirty_remove(extent_t *extent)
{

	qr_remove(&extent->rd, rd_link);
	qr_remove(extent, cc_link);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

