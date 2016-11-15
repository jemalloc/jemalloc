/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct extent_s extent_t;

#define	EXTENT_HOOKS_INITIALIZER	NULL

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

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

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern rtree_t			extents_rtree;
extern const extent_hooks_t	extent_hooks_default;

extent_t	*extent_alloc(tsdn_t *tsdn, arena_t *arena);
void	extent_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent);

extent_hooks_t	*extent_hooks_get(arena_t *arena);
extent_hooks_t	*extent_hooks_set(arena_t *arena, extent_hooks_t *extent_hooks);

#ifdef JEMALLOC_JET
typedef size_t (extent_size_quantize_t)(size_t);
extern extent_size_quantize_t *extent_size_quantize_floor;
extern extent_size_quantize_t *extent_size_quantize_ceil;
#else
size_t	extent_size_quantize_floor(size_t size);
size_t	extent_size_quantize_ceil(size_t size);
#endif

ph_proto(, extent_heap_, extent_heap_t, extent_t)

extent_t	*extent_alloc_cache_locked(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab);
extent_t	*extent_alloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab);
extent_t	*extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab);
void	extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, extent_t *extent);
void	extent_dalloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent);
void	extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent);
bool	extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool	extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool	extent_purge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
extent_t	*extent_split_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t size_a,
    size_t usize_a, size_t size_b, size_t usize_b);
bool	extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *a, extent_t *b);

bool	extent_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
extent_t	*extent_lookup(tsdn_t *tsdn, const void *ptr, bool dependent);
arena_t	*extent_arena_get(const extent_t *extent);
void	*extent_base_get(const extent_t *extent);
void	*extent_addr_get(const extent_t *extent);
size_t	extent_size_get(const extent_t *extent);
size_t	extent_usize_get(const extent_t *extent);
void	*extent_before_get(const extent_t *extent);
void	*extent_last_get(const extent_t *extent);
void	*extent_past_get(const extent_t *extent);
size_t	extent_sn_get(const extent_t *extent);
bool	extent_active_get(const extent_t *extent);
bool	extent_retained_get(const extent_t *extent);
bool	extent_zeroed_get(const extent_t *extent);
bool	extent_committed_get(const extent_t *extent);
bool	extent_slab_get(const extent_t *extent);
arena_slab_data_t	*extent_slab_data_get(extent_t *extent);
const arena_slab_data_t	*extent_slab_data_get_const(const extent_t *extent);
prof_tctx_t	*extent_prof_tctx_get(const extent_t *extent);
void	extent_arena_set(extent_t *extent, arena_t *arena);
void	extent_addr_set(extent_t *extent, void *addr);
void	extent_addr_randomize(tsdn_t *tsdn, extent_t *extent, size_t alignment);
void	extent_size_set(extent_t *extent, size_t size);
void	extent_usize_set(extent_t *extent, size_t usize);
void	extent_sn_set(extent_t *extent, size_t sn);
void	extent_active_set(extent_t *extent, bool active);
void	extent_zeroed_set(extent_t *extent, bool zeroed);
void	extent_committed_set(extent_t *extent, bool committed);
void	extent_slab_set(extent_t *extent, bool slab);
void	extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx);
void	extent_init(extent_t *extent, arena_t *arena, void *addr,
    size_t size, size_t usize, size_t sn, bool active, bool zeroed,
    bool committed, bool slab);
void	extent_ring_insert(extent_t *sentinel, extent_t *extent);
void	extent_ring_remove(extent_t *extent);
int	extent_sn_comp(const extent_t *a, const extent_t *b);
int	extent_ad_comp(const extent_t *a, const extent_t *b);
int	extent_snad_comp(const extent_t *a, const extent_t *b);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_EXTENT_C_))
JEMALLOC_INLINE extent_t *
extent_lookup(tsdn_t *tsdn, const void *ptr, bool dependent)
{
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	return (rtree_read(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    dependent));
}

JEMALLOC_INLINE arena_t *
extent_arena_get(const extent_t *extent)
{

	return (extent->e_arena);
}

JEMALLOC_INLINE void *
extent_base_get(const extent_t *extent)
{

	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent->e_slab);
	return (PAGE_ADDR2BASE(extent->e_addr));
}

JEMALLOC_INLINE void *
extent_addr_get(const extent_t *extent)
{

	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent->e_slab);
	return (extent->e_addr);
}

JEMALLOC_INLINE size_t
extent_size_get(const extent_t *extent)
{

	return (extent->e_size);
}

JEMALLOC_INLINE size_t
extent_usize_get(const extent_t *extent)
{

	assert(!extent->e_slab);
	return (extent->e_usize);
}

JEMALLOC_INLINE void *
extent_before_get(const extent_t *extent)
{

	return ((void *)((uintptr_t)extent_base_get(extent) - PAGE));
}

JEMALLOC_INLINE void *
extent_last_get(const extent_t *extent)
{

	return ((void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent) - PAGE));
}

JEMALLOC_INLINE void *
extent_past_get(const extent_t *extent)
{

	return ((void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent)));
}

JEMALLOC_INLINE size_t
extent_sn_get(const extent_t *extent)
{

	return (extent->e_sn);
}

JEMALLOC_INLINE bool
extent_active_get(const extent_t *extent)
{

	return (extent->e_active);
}

JEMALLOC_INLINE bool
extent_retained_get(const extent_t *extent)
{

	return (qr_next(extent, qr_link) == extent);
}

JEMALLOC_INLINE bool
extent_zeroed_get(const extent_t *extent)
{

	return (extent->e_zeroed);
}

JEMALLOC_INLINE bool
extent_committed_get(const extent_t *extent)
{

	return (extent->e_committed);
}

JEMALLOC_INLINE bool
extent_slab_get(const extent_t *extent)
{

	return (extent->e_slab);
}

JEMALLOC_INLINE arena_slab_data_t *
extent_slab_data_get(extent_t *extent)
{

	assert(extent->e_slab);
	return (&extent->e_slab_data);
}

JEMALLOC_INLINE const arena_slab_data_t *
extent_slab_data_get_const(const extent_t *extent)
{

	assert(extent->e_slab);
	return (&extent->e_slab_data);
}

JEMALLOC_INLINE prof_tctx_t *
extent_prof_tctx_get(const extent_t *extent)
{

	return ((prof_tctx_t *)atomic_read_p(
	    &((extent_t *)extent)->e_prof_tctx_pun));
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
extent_addr_randomize(tsdn_t *tsdn, extent_t *extent, size_t alignment)
{

	assert(extent_base_get(extent) == extent_addr_get(extent));

	if (alignment < PAGE) {
		unsigned lg_range = LG_PAGE -
		    lg_floor(CACHELINE_CEILING(alignment));
		size_t r =
		    prng_lg_range_zu(&extent_arena_get(extent)->offset_state,
		    lg_range, true);
		uintptr_t random_offset = ((uintptr_t)r) << (LG_PAGE -
		    lg_range);
		extent->e_addr = (void *)((uintptr_t)extent->e_addr +
		    random_offset);
		assert(ALIGNMENT_ADDR2BASE(extent->e_addr, alignment) ==
		    extent->e_addr);
	}
}

JEMALLOC_INLINE void
extent_size_set(extent_t *extent, size_t size)
{

	extent->e_size = size;
}

JEMALLOC_INLINE void
extent_usize_set(extent_t *extent, size_t usize)
{

	extent->e_usize = usize;
}

JEMALLOC_INLINE void
extent_sn_set(extent_t *extent, size_t sn)
{

	extent->e_sn = sn;
}

JEMALLOC_INLINE void
extent_active_set(extent_t *extent, bool active)
{

	extent->e_active = active;
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
extent_slab_set(extent_t *extent, bool slab)
{

	extent->e_slab = slab;
}

JEMALLOC_INLINE void
extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx)
{

	atomic_write_p(&extent->e_prof_tctx_pun, tctx);
}

JEMALLOC_INLINE void
extent_init(extent_t *extent, arena_t *arena, void *addr, size_t size,
    size_t usize, size_t sn, bool active, bool zeroed, bool committed,
    bool slab)
{

	assert(addr == PAGE_ADDR2BASE(addr) || !slab);

	extent_arena_set(extent, arena);
	extent_addr_set(extent, addr);
	extent_size_set(extent, size);
	extent_usize_set(extent, usize);
	extent_sn_set(extent, sn);
	extent_active_set(extent, active);
	extent_zeroed_set(extent, zeroed);
	extent_committed_set(extent, committed);
	extent_slab_set(extent, slab);
	if (config_prof)
		extent_prof_tctx_set(extent, NULL);
	qr_new(extent, qr_link);
}

JEMALLOC_INLINE void
extent_ring_insert(extent_t *sentinel, extent_t *extent)
{

	qr_meld(sentinel, extent, qr_link);
}

JEMALLOC_INLINE void
extent_ring_remove(extent_t *extent)
{

	qr_remove(extent, qr_link);
}

JEMALLOC_INLINE int
extent_sn_comp(const extent_t *a, const extent_t *b)
{
	size_t a_sn = extent_sn_get(a);
	size_t b_sn = extent_sn_get(b);

	return ((a_sn > b_sn) - (a_sn < b_sn));
}

JEMALLOC_INLINE int
extent_ad_comp(const extent_t *a, const extent_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

JEMALLOC_INLINE int
extent_snad_comp(const extent_t *a, const extent_t *b)
{
	int ret;

	ret = extent_sn_comp(a, b);
	if (ret != 0)
		return (ret);

	ret = extent_ad_comp(a, b);
	return (ret);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
