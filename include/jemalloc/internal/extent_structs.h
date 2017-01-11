#ifndef JEMALLOC_INTERNAL_EXTENT_STRUCTS_H
#define JEMALLOC_INTERNAL_EXTENT_STRUCTS_H

/* Extent (span of pages).  Use accessor functions for e_* fields. */
struct extent_s {
	/* Arena from which this extent came, if any. */
	arena_t			*e_arena;

	/* Pointer to the extent that this structure is responsible for. */
	void			*e_addr;

	/* Extent size. */
	size_t			e_size;

	/*
	 * Usable size, typically smaller than extent size due to large_pad or
	 * promotion of sampled small regions.
	 */
	size_t			e_usize;

	/*
	 * Serial number (potentially non-unique).
	 *
	 * In principle serial numbers can wrap around on 32-bit systems if
	 * JEMALLOC_MUNMAP is defined, but as long as comparison functions fall
	 * back on address comparison for equal serial numbers, stable (if
	 * imperfect) ordering is maintained.
	 *
	 * Serial numbers may not be unique even in the absence of wrap-around,
	 * e.g. when splitting an extent and assigning the same serial number to
	 * both resulting adjacent extents.
	 */
	size_t			e_sn;

	/* True if extent is active (in use). */
	bool			e_active;

	/*
	 * The zeroed flag is used by extent recycling code to track whether
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
	 * The slab flag indicates whether the extent is used for a slab of
	 * small regions.  This helps differentiate small size classes, and it
	 * indicates whether interior pointers can be looked up via iealloc().
	 */
	bool			e_slab;

	union {
		/* Small region slab metadata. */
		arena_slab_data_t	e_slab_data;

		/* Profile counters, used for large objects. */
		union {
			void		*e_prof_tctx_pun;
			prof_tctx_t	*e_prof_tctx;
		};
	};

	/*
	 * Linkage for arena's extents_dirty and arena_bin_t's slabs_full rings.
	 */
	qr(extent_t)		qr_link;

	union {
		/* Linkage for per size class sn/address-ordered heaps. */
		phn(extent_t)		ph_link;

		/* Linkage for arena's large and extent_cache lists. */
		ql_elm(extent_t)	ql_link;
	};
};
typedef ph(extent_t) extent_heap_t;

#endif /* JEMALLOC_INTERNAL_EXTENT_STRUCTS_H */
