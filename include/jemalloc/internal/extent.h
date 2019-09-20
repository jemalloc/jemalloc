#ifndef JEMALLOC_INTERNAL_EXTENT_H
#define JEMALLOC_INTERNAL_EXTENT_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bin_info.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/slab_data.h"
#include "jemalloc/internal/sz.h"

enum extent_state_e {
	extent_state_active   = 0,
	extent_state_dirty    = 1,
	extent_state_muzzy    = 2,
	extent_state_retained = 3
};
typedef enum extent_state_e extent_state_t;

enum extent_head_state_e {
	EXTENT_NOT_HEAD,
	EXTENT_IS_HEAD   /* Only relevant for Windows && opt.retain. */
};
typedef enum extent_head_state_e extent_head_state_t;

/* Extent (span of pages).  Use accessor functions for e_* fields. */
typedef struct extent_s extent_t;
typedef ql_head(extent_t) extent_list_t;
typedef ph(extent_t) extent_tree_t;
typedef ph(extent_t) extent_heap_t;
struct extent_s {
	/*
	 * Bitfield containing several fields:
	 *
	 * a: arena_ind
	 * b: slab
	 * c: committed
	 * d: dumpable
	 * z: zeroed
	 * t: state
	 * i: szind
	 * f: nfree
	 * s: bin_shard
	 * n: sn
	 *
	 * nnnnnnnn ... nnnnnnss ssssffff ffffffii iiiiiitt zdcbaaaa aaaaaaaa
	 *
	 * arena_ind: Arena from which this extent came, or all 1 bits if
	 *            unassociated.
	 *
	 * slab: The slab flag indicates whether the extent is used for a slab
	 *       of small regions.  This helps differentiate small size classes,
	 *       and it indicates whether interior pointers can be looked up via
	 *       iealloc().
	 *
	 * committed: The committed flag indicates whether physical memory is
	 *            committed to the extent, whether explicitly or implicitly
	 *            as on a system that overcommits and satisfies physical
	 *            memory needs on demand via soft page faults.
	 *
	 * dumpable: The dumpable flag indicates whether or not we've set the
	 *           memory in question to be dumpable.  Note that this
	 *           interacts somewhat subtly with user-specified extent hooks,
	 *           since we don't know if *they* are fiddling with
	 *           dumpability (in which case, we don't want to undo whatever
	 *           they're doing).  To deal with this scenario, we:
	 *             - Make dumpable false only for memory allocated with the
	 *               default hooks.
	 *             - Only allow memory to go from non-dumpable to dumpable,
	 *               and only once.
	 *             - Never make the OS call to allow dumping when the
	 *               dumpable bit is already set.
	 *           These three constraints mean that we will never
	 *           accidentally dump user memory that the user meant to set
	 *           nondumpable with their extent hooks.
	 *
	 *
	 * zeroed: The zeroed flag is used by extent recycling code to track
	 *         whether memory is zero-filled.
	 *
	 * state: The state flag is an extent_state_t.
	 *
	 * szind: The szind flag indicates usable size class index for
	 *        allocations residing in this extent, regardless of whether the
	 *        extent is a slab.  Extent size and usable size often differ
	 *        even for non-slabs, either due to sz_large_pad or promotion of
	 *        sampled small regions.
	 *
	 * nfree: Number of free regions in slab.
	 *
	 * bin_shard: the shard of the bin from which this extent came.
	 *
	 * sn: Serial number (potentially non-unique).
	 *
	 *     Serial numbers may wrap around if !opt_retain, but as long as
	 *     comparison functions fall back on address comparison for equal
	 *     serial numbers, stable (if imperfect) ordering is maintained.
	 *
	 *     Serial numbers may not be unique even in the absence of
	 *     wrap-around, e.g. when splitting an extent and assigning the same
	 *     serial number to both resulting adjacent extents.
	 */
	uint64_t		e_bits;
#define MASK(CURRENT_FIELD_WIDTH, CURRENT_FIELD_SHIFT) ((((((uint64_t)0x1U) << (CURRENT_FIELD_WIDTH)) - 1)) << (CURRENT_FIELD_SHIFT))

#define EXTENT_BITS_ARENA_WIDTH  MALLOCX_ARENA_BITS
#define EXTENT_BITS_ARENA_SHIFT  0
#define EXTENT_BITS_ARENA_MASK  MASK(EXTENT_BITS_ARENA_WIDTH, EXTENT_BITS_ARENA_SHIFT)

#define EXTENT_BITS_SLAB_WIDTH  1
#define EXTENT_BITS_SLAB_SHIFT  (EXTENT_BITS_ARENA_WIDTH + EXTENT_BITS_ARENA_SHIFT)
#define EXTENT_BITS_SLAB_MASK  MASK(EXTENT_BITS_SLAB_WIDTH, EXTENT_BITS_SLAB_SHIFT)

#define EXTENT_BITS_COMMITTED_WIDTH  1
#define EXTENT_BITS_COMMITTED_SHIFT  (EXTENT_BITS_SLAB_WIDTH + EXTENT_BITS_SLAB_SHIFT)
#define EXTENT_BITS_COMMITTED_MASK  MASK(EXTENT_BITS_COMMITTED_WIDTH, EXTENT_BITS_COMMITTED_SHIFT)

#define EXTENT_BITS_DUMPABLE_WIDTH  1
#define EXTENT_BITS_DUMPABLE_SHIFT  (EXTENT_BITS_COMMITTED_WIDTH + EXTENT_BITS_COMMITTED_SHIFT)
#define EXTENT_BITS_DUMPABLE_MASK  MASK(EXTENT_BITS_DUMPABLE_WIDTH, EXTENT_BITS_DUMPABLE_SHIFT)

#define EXTENT_BITS_ZEROED_WIDTH  1
#define EXTENT_BITS_ZEROED_SHIFT  (EXTENT_BITS_DUMPABLE_WIDTH + EXTENT_BITS_DUMPABLE_SHIFT)
#define EXTENT_BITS_ZEROED_MASK  MASK(EXTENT_BITS_ZEROED_WIDTH, EXTENT_BITS_ZEROED_SHIFT)

#define EXTENT_BITS_STATE_WIDTH  2
#define EXTENT_BITS_STATE_SHIFT  (EXTENT_BITS_ZEROED_WIDTH + EXTENT_BITS_ZEROED_SHIFT)
#define EXTENT_BITS_STATE_MASK  MASK(EXTENT_BITS_STATE_WIDTH, EXTENT_BITS_STATE_SHIFT)

#define EXTENT_BITS_SZIND_WIDTH  LG_CEIL(SC_NSIZES)
#define EXTENT_BITS_SZIND_SHIFT  (EXTENT_BITS_STATE_WIDTH + EXTENT_BITS_STATE_SHIFT)
#define EXTENT_BITS_SZIND_MASK  MASK(EXTENT_BITS_SZIND_WIDTH, EXTENT_BITS_SZIND_SHIFT)

#define EXTENT_BITS_NFREE_WIDTH  (SC_LG_SLAB_MAXREGS + 1)
#define EXTENT_BITS_NFREE_SHIFT  (EXTENT_BITS_SZIND_WIDTH + EXTENT_BITS_SZIND_SHIFT)
#define EXTENT_BITS_NFREE_MASK  MASK(EXTENT_BITS_NFREE_WIDTH, EXTENT_BITS_NFREE_SHIFT)

#define EXTENT_BITS_BINSHARD_WIDTH  6
#define EXTENT_BITS_BINSHARD_SHIFT  (EXTENT_BITS_NFREE_WIDTH + EXTENT_BITS_NFREE_SHIFT)
#define EXTENT_BITS_BINSHARD_MASK  MASK(EXTENT_BITS_BINSHARD_WIDTH, EXTENT_BITS_BINSHARD_SHIFT)

#define EXTENT_BITS_IS_HEAD_WIDTH 1
#define EXTENT_BITS_IS_HEAD_SHIFT  (EXTENT_BITS_BINSHARD_WIDTH + EXTENT_BITS_BINSHARD_SHIFT)
#define EXTENT_BITS_IS_HEAD_MASK  MASK(EXTENT_BITS_IS_HEAD_WIDTH, EXTENT_BITS_IS_HEAD_SHIFT)

#define EXTENT_BITS_SN_SHIFT   (EXTENT_BITS_IS_HEAD_WIDTH + EXTENT_BITS_IS_HEAD_SHIFT)
#define EXTENT_BITS_SN_MASK  (UINT64_MAX << EXTENT_BITS_SN_SHIFT)

	/* Pointer to the extent that this structure is responsible for. */
	void			*e_addr;

	union {
		/*
		 * Extent size and serial number associated with the extent
		 * structure (different than the serial number for the extent at
		 * e_addr).
		 *
		 * ssssssss [...] ssssssss ssssnnnn nnnnnnnn
		 */
		size_t			e_size_esn;
	#define EXTENT_SIZE_MASK	((size_t)~(PAGE-1))
	#define EXTENT_ESN_MASK		((size_t)PAGE-1)
		/* Base extent size, which may not be a multiple of PAGE. */
		size_t			e_bsize;
	};

	/*
	 * List linkage, used by a variety of lists:
	 * - bin_t's slabs_full
	 * - extents_t's LRU
	 * - stashed dirty extents
	 * - arena's large allocations
	 */
	ql_elm(extent_t)	ql_link;

	/*
	 * Linkage for per size class sn/address-ordered heaps, and
	 * for extent_avail
	 */
	phn(extent_t)		ph_link;

	union {
		/* Small region slab metadata. */
		slab_data_t	e_slab_data;

		/* Profiling data, used for large objects. */
		struct {
			/* Time when this was allocated. */
			nstime_t		e_alloc_time;
			/* Points to a prof_tctx_t. */
			atomic_p_t		e_prof_tctx;
		};
	};
};

static inline unsigned
extent_arena_ind_get(const extent_t *extent) {
	unsigned arena_ind = (unsigned)((extent->e_bits &
	    EXTENT_BITS_ARENA_MASK) >> EXTENT_BITS_ARENA_SHIFT);
	assert(arena_ind < MALLOCX_ARENA_LIMIT);

	return arena_ind;
}

static inline szind_t
extent_szind_get_maybe_invalid(const extent_t *extent) {
	szind_t szind = (szind_t)((extent->e_bits & EXTENT_BITS_SZIND_MASK) >>
	    EXTENT_BITS_SZIND_SHIFT);
	assert(szind <= SC_NSIZES);
	return szind;
}

static inline szind_t
extent_szind_get(const extent_t *extent) {
	szind_t szind = extent_szind_get_maybe_invalid(extent);
	assert(szind < SC_NSIZES); /* Never call when "invalid". */
	return szind;
}

static inline size_t
extent_usize_get(const extent_t *extent) {
	return sz_index2size(extent_szind_get(extent));
}

static inline unsigned
extent_binshard_get(const extent_t *extent) {
	unsigned binshard = (unsigned)((extent->e_bits &
	    EXTENT_BITS_BINSHARD_MASK) >> EXTENT_BITS_BINSHARD_SHIFT);
	assert(binshard < bin_infos[extent_szind_get(extent)].n_shards);
	return binshard;
}

static inline size_t
extent_sn_get(const extent_t *extent) {
	return (size_t)((extent->e_bits & EXTENT_BITS_SN_MASK) >>
	    EXTENT_BITS_SN_SHIFT);
}

static inline extent_state_t
extent_state_get(const extent_t *extent) {
	return (extent_state_t)((extent->e_bits & EXTENT_BITS_STATE_MASK) >>
	    EXTENT_BITS_STATE_SHIFT);
}

static inline bool
extent_zeroed_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_ZEROED_MASK) >>
	    EXTENT_BITS_ZEROED_SHIFT);
}

static inline bool
extent_committed_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_COMMITTED_MASK) >>
	    EXTENT_BITS_COMMITTED_SHIFT);
}

static inline bool
extent_dumpable_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_DUMPABLE_MASK) >>
	    EXTENT_BITS_DUMPABLE_SHIFT);
}

static inline bool
extent_slab_get(const extent_t *extent) {
	return (bool)((extent->e_bits & EXTENT_BITS_SLAB_MASK) >>
	    EXTENT_BITS_SLAB_SHIFT);
}

static inline unsigned
extent_nfree_get(const extent_t *extent) {
	assert(extent_slab_get(extent));
	return (unsigned)((extent->e_bits & EXTENT_BITS_NFREE_MASK) >>
	    EXTENT_BITS_NFREE_SHIFT);
}

static inline void *
extent_base_get(const extent_t *extent) {
	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent_slab_get(extent));
	return PAGE_ADDR2BASE(extent->e_addr);
}

static inline void *
extent_addr_get(const extent_t *extent) {
	assert(extent->e_addr == PAGE_ADDR2BASE(extent->e_addr) ||
	    !extent_slab_get(extent));
	return extent->e_addr;
}

static inline size_t
extent_size_get(const extent_t *extent) {
	return (extent->e_size_esn & EXTENT_SIZE_MASK);
}

static inline size_t
extent_esn_get(const extent_t *extent) {
	return (extent->e_size_esn & EXTENT_ESN_MASK);
}

static inline size_t
extent_bsize_get(const extent_t *extent) {
	return extent->e_bsize;
}

static inline void *
extent_before_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) - PAGE);
}

static inline void *
extent_last_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent) - PAGE);
}

static inline void *
extent_past_get(const extent_t *extent) {
	return (void *)((uintptr_t)extent_base_get(extent) +
	    extent_size_get(extent));
}

static inline slab_data_t *
extent_slab_data_get(extent_t *extent) {
	assert(extent_slab_get(extent));
	return &extent->e_slab_data;
}

static inline const slab_data_t *
extent_slab_data_get_const(const extent_t *extent) {
	assert(extent_slab_get(extent));
	return &extent->e_slab_data;
}

static inline prof_tctx_t *
extent_prof_tctx_get(const extent_t *extent) {
	return (prof_tctx_t *)atomic_load_p(&extent->e_prof_tctx,
	    ATOMIC_ACQUIRE);
}

static inline nstime_t
extent_prof_alloc_time_get(const extent_t *extent) {
	return extent->e_alloc_time;
}

static inline void
extent_arena_ind_set(extent_t *extent, unsigned arena_ind) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_ARENA_MASK) |
	    ((uint64_t)arena_ind << EXTENT_BITS_ARENA_SHIFT);
}

static inline void
extent_binshard_set(extent_t *extent, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[extent_szind_get(extent)].n_shards);
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_BINSHARD_MASK) |
	    ((uint64_t)binshard << EXTENT_BITS_BINSHARD_SHIFT);
}

static inline void
extent_addr_set(extent_t *extent, void *addr) {
	extent->e_addr = addr;
}

static inline void
extent_size_set(extent_t *extent, size_t size) {
	assert((size & ~EXTENT_SIZE_MASK) == 0);
	extent->e_size_esn = size | (extent->e_size_esn & ~EXTENT_SIZE_MASK);
}

static inline void
extent_esn_set(extent_t *extent, size_t esn) {
	extent->e_size_esn = (extent->e_size_esn & ~EXTENT_ESN_MASK) | (esn &
	    EXTENT_ESN_MASK);
}

static inline void
extent_bsize_set(extent_t *extent, size_t bsize) {
	extent->e_bsize = bsize;
}

static inline void
extent_szind_set(extent_t *extent, szind_t szind) {
	assert(szind <= SC_NSIZES); /* SC_NSIZES means "invalid". */
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SZIND_MASK) |
	    ((uint64_t)szind << EXTENT_BITS_SZIND_SHIFT);
}

static inline void
extent_nfree_set(extent_t *extent, unsigned nfree) {
	assert(extent_slab_get(extent));
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_NFREE_MASK) |
	    ((uint64_t)nfree << EXTENT_BITS_NFREE_SHIFT);
}

static inline void
extent_nfree_binshard_set(extent_t *extent, unsigned nfree, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[extent_szind_get(extent)].n_shards);
	extent->e_bits = (extent->e_bits &
	    (~EXTENT_BITS_NFREE_MASK & ~EXTENT_BITS_BINSHARD_MASK)) |
	    ((uint64_t)binshard << EXTENT_BITS_BINSHARD_SHIFT) |
	    ((uint64_t)nfree << EXTENT_BITS_NFREE_SHIFT);
}

static inline void
extent_nfree_inc(extent_t *extent) {
	assert(extent_slab_get(extent));
	extent->e_bits += ((uint64_t)1U << EXTENT_BITS_NFREE_SHIFT);
}

static inline void
extent_nfree_dec(extent_t *extent) {
	assert(extent_slab_get(extent));
	extent->e_bits -= ((uint64_t)1U << EXTENT_BITS_NFREE_SHIFT);
}

static inline void
extent_nfree_sub(extent_t *extent, uint64_t n) {
	assert(extent_slab_get(extent));
	extent->e_bits -= (n << EXTENT_BITS_NFREE_SHIFT);
}

static inline void
extent_sn_set(extent_t *extent, size_t sn) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SN_MASK) |
	    ((uint64_t)sn << EXTENT_BITS_SN_SHIFT);
}

static inline void
extent_state_set(extent_t *extent, extent_state_t state) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_STATE_MASK) |
	    ((uint64_t)state << EXTENT_BITS_STATE_SHIFT);
}

static inline void
extent_zeroed_set(extent_t *extent, bool zeroed) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_ZEROED_MASK) |
	    ((uint64_t)zeroed << EXTENT_BITS_ZEROED_SHIFT);
}

static inline void
extent_committed_set(extent_t *extent, bool committed) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_COMMITTED_MASK) |
	    ((uint64_t)committed << EXTENT_BITS_COMMITTED_SHIFT);
}

static inline void
extent_dumpable_set(extent_t *extent, bool dumpable) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_DUMPABLE_MASK) |
	    ((uint64_t)dumpable << EXTENT_BITS_DUMPABLE_SHIFT);
}

static inline void
extent_slab_set(extent_t *extent, bool slab) {
	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_SLAB_MASK) |
	    ((uint64_t)slab << EXTENT_BITS_SLAB_SHIFT);
}

static inline void
extent_prof_tctx_set(extent_t *extent, prof_tctx_t *tctx) {
	atomic_store_p(&extent->e_prof_tctx, tctx, ATOMIC_RELEASE);
}

static inline void
extent_prof_alloc_time_set(extent_t *extent, nstime_t t) {
	nstime_copy(&extent->e_alloc_time, &t);
}

static inline bool
extent_is_head_get(extent_t *extent) {
	if (maps_coalesce) {
		not_reached();
	}

	return (bool)((extent->e_bits & EXTENT_BITS_IS_HEAD_MASK) >>
	    EXTENT_BITS_IS_HEAD_SHIFT);
}

static inline void
extent_is_head_set(extent_t *extent, bool is_head) {
	if (maps_coalesce) {
		not_reached();
	}

	extent->e_bits = (extent->e_bits & ~EXTENT_BITS_IS_HEAD_MASK) |
	    ((uint64_t)is_head << EXTENT_BITS_IS_HEAD_SHIFT);
}

static inline void
extent_init(extent_t *extent, unsigned arena_ind, void *addr, size_t size,
    bool slab, szind_t szind, size_t sn, extent_state_t state, bool zeroed,
    bool committed, bool dumpable, extent_head_state_t is_head) {
	assert(addr == PAGE_ADDR2BASE(addr) || !slab);

	extent_arena_ind_set(extent, arena_ind);
	extent_addr_set(extent, addr);
	extent_size_set(extent, size);
	extent_slab_set(extent, slab);
	extent_szind_set(extent, szind);
	extent_sn_set(extent, sn);
	extent_state_set(extent, state);
	extent_zeroed_set(extent, zeroed);
	extent_committed_set(extent, committed);
	extent_dumpable_set(extent, dumpable);
	ql_elm_new(extent, ql_link);
	if (!maps_coalesce) {
		extent_is_head_set(extent, (is_head == EXTENT_IS_HEAD) ? true :
		    false);
	}
	if (config_prof) {
		extent_prof_tctx_set(extent, NULL);
	}
}

static inline void
extent_binit(extent_t *extent, void *addr, size_t bsize, size_t sn) {
	extent_arena_ind_set(extent, (1U << MALLOCX_ARENA_BITS) - 1);
	extent_addr_set(extent, addr);
	extent_bsize_set(extent, bsize);
	extent_slab_set(extent, false);
	extent_szind_set(extent, SC_NSIZES);
	extent_sn_set(extent, sn);
	extent_state_set(extent, extent_state_active);
	extent_zeroed_set(extent, true);
	extent_committed_set(extent, true);
	extent_dumpable_set(extent, true);
}

static inline void
extent_list_init(extent_list_t *list) {
	ql_new(list);
}

static inline extent_t *
extent_list_first(const extent_list_t *list) {
	return ql_first(list);
}

static inline extent_t *
extent_list_last(const extent_list_t *list) {
	return ql_last(list, ql_link);
}

static inline void
extent_list_append(extent_list_t *list, extent_t *extent) {
	ql_tail_insert(list, extent, ql_link);
}

static inline void
extent_list_prepend(extent_list_t *list, extent_t *extent) {
	ql_head_insert(list, extent, ql_link);
}

static inline void
extent_list_replace(extent_list_t *list, extent_t *to_remove,
    extent_t *to_insert) {
	ql_after_insert(to_remove, to_insert, ql_link);
	ql_remove(list, to_remove, ql_link);
}

static inline void
extent_list_remove(extent_list_t *list, extent_t *extent) {
	ql_remove(list, extent, ql_link);
}

static inline int
extent_sn_comp(const extent_t *a, const extent_t *b) {
	size_t a_sn = extent_sn_get(a);
	size_t b_sn = extent_sn_get(b);

	return (a_sn > b_sn) - (a_sn < b_sn);
}

static inline int
extent_esn_comp(const extent_t *a, const extent_t *b) {
	size_t a_esn = extent_esn_get(a);
	size_t b_esn = extent_esn_get(b);

	return (a_esn > b_esn) - (a_esn < b_esn);
}

static inline int
extent_ad_comp(const extent_t *a, const extent_t *b) {
	uintptr_t a_addr = (uintptr_t)extent_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_addr_get(b);

	return (a_addr > b_addr) - (a_addr < b_addr);
}

static inline int
extent_ead_comp(const extent_t *a, const extent_t *b) {
	uintptr_t a_eaddr = (uintptr_t)a;
	uintptr_t b_eaddr = (uintptr_t)b;

	return (a_eaddr > b_eaddr) - (a_eaddr < b_eaddr);
}

static inline int
extent_snad_comp(const extent_t *a, const extent_t *b) {
	int ret;

	ret = extent_sn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = extent_ad_comp(a, b);
	return ret;
}

static inline int
extent_esnead_comp(const extent_t *a, const extent_t *b) {
	int ret;

	ret = extent_esn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = extent_ead_comp(a, b);
	return ret;
}

#endif /* JEMALLOC_INTERNAL_EXTENT_H */
