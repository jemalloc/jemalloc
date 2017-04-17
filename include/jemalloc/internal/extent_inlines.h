#ifndef JEMALLOC_INTERNAL_EXTENT_INLINES_H
#define JEMALLOC_INTERNAL_EXTENT_INLINES_H

#include "jemalloc/internal/ql.h"

#ifndef JEMALLOC_ENABLE_INLINE
arena_t *extent_arena_get(const extent_t *extent);
szind_t extent_szind_get_maybe_invalid(const extent_t *extent);
szind_t extent_szind_get(const extent_t *extent);
size_t extent_usize_get(const extent_t *extent);
size_t extent_sn_get(const extent_t *extent);
extent_state_t extent_state_get(const extent_t *extent);
bool extent_zeroed_get(const extent_t *extent);
bool extent_committed_get(const extent_t *extent);
bool extent_slab_get(const extent_t *extent);
unsigned extent_nfree_get(const extent_t *extent);
void *extent_base_get(const extent_t *extent);
void *extent_addr_get(const extent_t *extent);
size_t extent_size_get(const extent_t *extent);
size_t extent_esn_get(const extent_t *extent);
size_t extent_bsize_get(const extent_t *extent);
void *extent_before_get(const extent_t *extent);
void *extent_last_get(const extent_t *extent);
void *extent_past_get(const extent_t *extent);
arena_slab_data_t *extent_slab_data_get(extent_t *extent);
const arena_slab_data_t *extent_slab_data_get_const(const extent_t *extent);
prof_tctx_t *extent_prof_tctx_get(const extent_t *extent);
void extent_arena_set(extent_t *extent, arena_t *arena);
void extent_addr_set(extent_t *extent, void *addr);
void extent_addr_randomize(tsdn_t *tsdn, extent_t *extent, size_t alignment);
void extent_size_set(extent_t *extent, size_t size);
void extent_esn_set(extent_t *extent, size_t esn);
void extent_bsize_set(extent_t *extent, size_t bsize);
void extent_szind_set(extent_t *extent, szind_t szind);
void extent_nfree_set(extent_t *extent, unsigned nfree);
void extent_nfree_inc(extent_t *extent);
void extent_nfree_dec(extent_t *extent);
void extent_sn_set(extent_t *extent, size_t sn);
void extent_state_set(extent_t *extent, extent_state_t state);
void extent_zeroed_set(extent_t *extent, bool zeroed);
void extent_committed_set(extent_t *extent, bool committed);
void extent_slab_set(extent_t *extent, bool slab);
void extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx);
void extent_init(extent_t *extent, arena_t *arena, void *addr, size_t size,
    bool slab, szind_t szind, size_t sn, extent_state_t state, bool zeroed,
    bool committed);
void extent_binit(extent_t *extent, void *addr, size_t size, size_t sn);
void extent_list_init(extent_list_t *list);
extent_t *extent_list_first(const extent_list_t *list);
extent_t *extent_list_last(const extent_list_t *list);
void extent_list_append(extent_list_t *list, extent_t *extent);
void extent_list_replace(extent_list_t *list, extent_t *to_remove,
    extent_t *to_insert);
void extent_list_remove(extent_list_t *list, extent_t *extent);
int extent_sn_comp(const extent_t *a, const extent_t *b);
int extent_esn_comp(const extent_t *a, const extent_t *b);
int extent_ad_comp(const extent_t *a, const extent_t *b);
int extent_snad_comp(const extent_t *a, const extent_t *b);
int extent_esnead_comp(const extent_t *a, const extent_t *b);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_EXTENT_C_))
JEMALLOC_INLINE arena_t *
extent_arena_get(const extent_t *extent) {
	unsigned arena_ind = (unsigned)((extent->e_bits &
	    EXTENT_BITS_ARENA_MASK) >> EXTENT_BITS_ARENA_SHIFT);
	/*
	 * The following check is omitted because we should never actually read
	 * a NULL arena pointer.
	 */
	if (false && arena_ind > MALLOCX_ARENA_MAX) {
		return NULL;
	}
	assert(arena_ind <= MALLOCX_ARENA_MAX);
	return (arena_t *)atomic_load_p(&arenas[arena_ind], ATOMIC_ACQUIRE);
}

JEMALLOC_INLINE szind_t
extent_szind_get_maybe_invalid(const extent_t *extent) {
	szind_t szind = (szind_t)((extent->e_bits & EXTENT_BITS_SZIND_MASK) >>
	    EXTENT_BITS_SZIND_SHIFT);
	assert(szind <= NSIZES);
	return szind;
}

JEMALLOC_INLINE szind_t
extent_szind_get(const extent_t *extent) {
	szind_t szind = extent_szind_get_maybe_invalid(extent);
	assert(szind < NSIZES); /* Never call when "invalid". */
	return szind;
}

JEMALLOC_INLINE size_t
extent_usize_get(const extent_t *extent) {
	return index2size(extent_szind_get(extent));
}

JEMALLOC_INLINE size_t
extent_sn_get(const extent_t *extent) {
	return (size_t)((extent->e_bits & EXTENT_BITS_SN_MASK) >>
	    EXTENT_BITS_SN_SHIFT);
}

JEMALLOC_INLINE extent_state_t
extent_state_get(const extent_t *extent) {
	return (extent_state_t)((extent->e_bits & EXTENT_BITS_STATE_MASK) >>
	    EXTENT_BITS_STATE_SHIFT);
}

JEMALLOC_INLINE bool
extent_zeroed_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_ZEROED_MASK) >>
	    EXTENT_BITS_ZEROED_SHIFT);
}

JEMALLOC_INLINE bool
extent_committed_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_COMMITTED_MASK) >>
	    EXTENT_BITS_COMMITTED_SHIFT);
}

JEMALLOC_INLINE bool
extent_slab_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_SLAB_MASK) >>
	    EXTENT_BITS_SLAB_SHIFT);
}

JEMALLOC_INLINE unsigned
extent_nfree_get(const extent_t *extent) {
	assert(extent_slab_get(extent));
	return (unsigned)((extent->e_bits & EXTENT_BITS_NFREE_MASK) >>
	    EXTENT_BITS_NFREE_SHIFT);
}

JEMALLOC_INLINE void *
extent_base_get(const extent_t *extent) {
	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent_slab_get(extent));
	return PAGE_ADDR2BASE(extent->e_addr);
}

JEMALLOC_INLINE void *
extent_addr_get(const extent_t *extent) {
	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent_slab_get(extent));
	return extent->e_addr;
}

JEMALLOC_INLINE size_t
extent_size_get(const extent_t *extent) {
	return (extent->e_size_esn & EXTENT_SIZE_MASK);
}

JEMALLOC_INLINE size_t
extent_esn_get(const extent_t *extent) {
	return (extent->e_size_esn & EXTENT_ESN_MASK);
}

JEMALLOC_INLINE size_t
extent_bsize_get(const extent_t *extent) {
	return extent->e_bsize;
}

JEMALLOC_INLINE void *
extent_before_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) - PAGE);
}

JEMALLOC_INLINE void *
extent_last_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent) - PAGE);
}

JEMALLOC_INLINE void *
extent_past_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent));
}

JEMALLOC_INLINE arena_slab_data_t *
extent_slab_data_get(extent_t *extent) {
	assert(extent_slab_get(extent));
	return &extent->e_slab_data;
}

JEMALLOC_INLINE const arena_slab_data_t *
extent_slab_data_get_const(const extent_t *extent) {
	assert(extent_slab_get(extent));
	return &extent->e_slab_data;
}

JEMALLOC_INLINE prof_tctx_t *
extent_prof_tctx_get(const extent_t *extent) {
	return (prof_tctx_t *)atomic_load_p(&extent->e_prof_tctx,
	    ATOMIC_ACQUIRE);
}

JEMALLOC_INLINE void
extent_arena_set(extent_t *extent, arena_t *arena) {
	unsigned arena_ind = (arena != NULL) ? arena_ind_get(arena) : ((1U <<
	    MALLOCX_ARENA_BITS) - 1);
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_ARENA_MASK) |
	    ((uint64_t)arena_ind << EXTENT_BITS_ARENA_SHIFT);
}

JEMALLOC_INLINE void
extent_addr_set(extent_t *extent, void *addr) {
	extent->e_addr = addr;
}

JEMALLOC_INLINE void
extent_addr_randomize(tsdn_t *tsdn, extent_t *extent, size_t alignment) {
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
extent_size_set(extent_t *extent, size_t size) {
	assert((size & ~EXTENT_SIZE_MASK) == 0);
	extent->e_size_esn = size | (extent->e_size_esn & ~EXTENT_SIZE_MASK);
}

JEMALLOC_INLINE void
extent_esn_set(extent_t *extent, size_t esn) {
	extent->e_size_esn = (extent->e_size_esn & ~EXTENT_ESN_MASK) | (esn &
	    EXTENT_ESN_MASK);
}

JEMALLOC_INLINE void
extent_bsize_set(extent_t *extent, size_t bsize) {
	extent->e_bsize = bsize;
}

JEMALLOC_INLINE void
extent_szind_set(extent_t *extent, szind_t szind) {
	assert(szind <= NSIZES); /* NSIZES means "invalid". */
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SZIND_MASK) |
	    ((uint64_t)szind << EXTENT_BITS_SZIND_SHIFT);
}

JEMALLOC_INLINE void
extent_nfree_set(extent_t *extent, unsigned nfree) {
	assert(extent_slab_get(extent));
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_NFREE_MASK) |
	    ((uint64_t)nfree << EXTENT_BITS_NFREE_SHIFT);
}

JEMALLOC_INLINE void
extent_nfree_inc(extent_t *extent) {
	assert(extent_slab_get(extent));
	extent->e_bits += ((uint64_t)1U << EXTENT_BITS_NFREE_SHIFT);
}

JEMALLOC_INLINE void
extent_nfree_dec(extent_t *extent) {
	assert(extent_slab_get(extent));
	extent->e_bits -= ((uint64_t)1U << EXTENT_BITS_NFREE_SHIFT);
}

JEMALLOC_INLINE void
extent_sn_set(extent_t *extent, size_t sn) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SN_MASK) |
	    ((uint64_t)sn << EXTENT_BITS_SN_SHIFT);
}

JEMALLOC_INLINE void
extent_state_set(extent_t *extent, extent_state_t state) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_STATE_MASK) |
	    ((uint64_t)state << EXTENT_BITS_STATE_SHIFT);
}

JEMALLOC_INLINE void
extent_zeroed_set(extent_t *extent, bool zeroed) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_ZEROED_MASK) |
	    ((uint64_t)zeroed << EXTENT_BITS_ZEROED_SHIFT);
}

JEMALLOC_INLINE void
extent_committed_set(extent_t *extent, bool committed) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_COMMITTED_MASK) |
	    ((uint64_t)committed << EXTENT_BITS_COMMITTED_SHIFT);
}

JEMALLOC_INLINE void
extent_slab_set(extent_t *extent, bool slab) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SLAB_MASK) |
	    ((uint64_t)slab << EXTENT_BITS_SLAB_SHIFT);
}

JEMALLOC_INLINE void
extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx) {
	atomic_store_p(&extent->e_prof_tctx, tctx, ATOMIC_RELEASE);
}

JEMALLOC_INLINE void
extent_init(extent_t *extent, arena_t *arena, void *addr, size_t size,
    bool slab, szind_t szind, size_t sn, extent_state_t state, bool zeroed,
    bool committed) {
	assert(addr == PAGE_ADDR2BASE(addr) || !slab);

	extent_arena_set(extent, arena);
	extent_addr_set(extent, addr);
	extent_size_set(extent, size);
	extent_slab_set(extent, slab);
	extent_szind_set(extent, szind);
	extent_sn_set(extent, sn);
	extent_state_set(extent, state);
	extent_zeroed_set(extent, zeroed);
	extent_committed_set(extent, committed);
	ql_elm_new(extent, ql_link);
	if (config_prof) {
		extent_prof_tctx_set(extent, NULL);
	}
}

JEMALLOC_INLINE void
extent_binit(extent_t *extent, void *addr, size_t bsize, size_t sn) {
	extent_arena_set(extent, NULL);
	extent_addr_set(extent, addr);
	extent_bsize_set(extent, bsize);
	extent_slab_set(extent, false);
	extent_szind_set(extent, NSIZES);
	extent_sn_set(extent, sn);
	extent_state_set(extent, extent_state_active);
	extent_zeroed_set(extent, true);
	extent_committed_set(extent, true);
}

JEMALLOC_INLINE void
extent_list_init(extent_list_t *list) {
	ql_new(list);
}

JEMALLOC_INLINE extent_t *
extent_list_first(const extent_list_t *list) {
	return ql_first(list);
}

JEMALLOC_INLINE extent_t *
extent_list_last(const extent_list_t *list) {
	return ql_last(list, ql_link);
}

JEMALLOC_INLINE void
extent_list_append(extent_list_t *list, extent_t *extent) {
	ql_tail_insert(list, extent, ql_link);
}

JEMALLOC_INLINE void
extent_list_replace(extent_list_t *list, extent_t *to_remove,
    extent_t *to_insert) {
	ql_after_insert(to_remove, to_insert, ql_link);
	ql_remove(list, to_remove, ql_link);
}

JEMALLOC_INLINE void
extent_list_remove(extent_list_t *list, extent_t *extent) {
	ql_remove(list, extent, ql_link);
}

JEMALLOC_INLINE int
extent_sn_comp(const extent_t *a, const extent_t *b) {
	size_t a_sn = extent_sn_get(a);
	size_t b_sn = extent_sn_get(b);

	return (a_sn > b_sn) - (a_sn < b_sn);
}

JEMALLOC_INLINE int
extent_esn_comp(const extent_t *a, const extent_t *b) {
	size_t a_esn = extent_esn_get(a);
	size_t b_esn = extent_esn_get(b);

	return (a_esn > b_esn) - (a_esn < b_esn);
}

JEMALLOC_INLINE int
extent_ad_comp(const extent_t *a, const extent_t *b) {
	uintptr_t a_addr = (uintptr_t)extent_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_addr_get(b);

	return (a_addr > b_addr) - (a_addr < b_addr);
}

JEMALLOC_INLINE int
extent_ead_comp(const extent_t *a, const extent_t *b) {
	uintptr_t a_eaddr = (uintptr_t)a;
	uintptr_t b_eaddr = (uintptr_t)b;

	return (a_eaddr > b_eaddr) - (a_eaddr < b_eaddr);
}

JEMALLOC_INLINE int
extent_snad_comp(const extent_t *a, const extent_t *b) {
	int ret;

	ret = extent_sn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = extent_ad_comp(a, b);
	return ret;
}

JEMALLOC_INLINE int
extent_esnead_comp(const extent_t *a, const extent_t *b) {
	int ret;

	ret = extent_esn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = extent_ead_comp(a, b);
	return ret;
}
#endif

#endif /* JEMALLOC_INTERNAL_EXTENT_INLINES_H */
