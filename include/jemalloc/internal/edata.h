#ifndef JEMALLOC_INTERNAL_EDATA_H
#define JEMALLOC_INTERNAL_EDATA_H

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
typedef struct edata_s edata_t;
typedef ql_head(edata_t) edata_list_t;
typedef ph(edata_t) edata_tree_t;
typedef ph(edata_t) edata_heap_t;
struct edata_s {
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

#define EDATA_BITS_ARENA_WIDTH  MALLOCX_ARENA_BITS
#define EDATA_BITS_ARENA_SHIFT  0
#define EDATA_BITS_ARENA_MASK  MASK(EDATA_BITS_ARENA_WIDTH, EDATA_BITS_ARENA_SHIFT)

#define EDATA_BITS_SLAB_WIDTH  1
#define EDATA_BITS_SLAB_SHIFT  (EDATA_BITS_ARENA_WIDTH + EDATA_BITS_ARENA_SHIFT)
#define EDATA_BITS_SLAB_MASK  MASK(EDATA_BITS_SLAB_WIDTH, EDATA_BITS_SLAB_SHIFT)

#define EDATA_BITS_COMMITTED_WIDTH  1
#define EDATA_BITS_COMMITTED_SHIFT  (EDATA_BITS_SLAB_WIDTH + EDATA_BITS_SLAB_SHIFT)
#define EDATA_BITS_COMMITTED_MASK  MASK(EDATA_BITS_COMMITTED_WIDTH, EDATA_BITS_COMMITTED_SHIFT)

#define EDATA_BITS_DUMPABLE_WIDTH  1
#define EDATA_BITS_DUMPABLE_SHIFT  (EDATA_BITS_COMMITTED_WIDTH + EDATA_BITS_COMMITTED_SHIFT)
#define EDATA_BITS_DUMPABLE_MASK  MASK(EDATA_BITS_DUMPABLE_WIDTH, EDATA_BITS_DUMPABLE_SHIFT)

#define EDATA_BITS_ZEROED_WIDTH  1
#define EDATA_BITS_ZEROED_SHIFT  (EDATA_BITS_DUMPABLE_WIDTH + EDATA_BITS_DUMPABLE_SHIFT)
#define EDATA_BITS_ZEROED_MASK  MASK(EDATA_BITS_ZEROED_WIDTH, EDATA_BITS_ZEROED_SHIFT)

#define EDATA_BITS_STATE_WIDTH  2
#define EDATA_BITS_STATE_SHIFT  (EDATA_BITS_ZEROED_WIDTH + EDATA_BITS_ZEROED_SHIFT)
#define EDATA_BITS_STATE_MASK  MASK(EDATA_BITS_STATE_WIDTH, EDATA_BITS_STATE_SHIFT)

#define EDATA_BITS_SZIND_WIDTH  LG_CEIL(SC_NSIZES)
#define EDATA_BITS_SZIND_SHIFT  (EDATA_BITS_STATE_WIDTH + EDATA_BITS_STATE_SHIFT)
#define EDATA_BITS_SZIND_MASK  MASK(EDATA_BITS_SZIND_WIDTH, EDATA_BITS_SZIND_SHIFT)

#define EDATA_BITS_NFREE_WIDTH  (SC_LG_SLAB_MAXREGS + 1)
#define EDATA_BITS_NFREE_SHIFT  (EDATA_BITS_SZIND_WIDTH + EDATA_BITS_SZIND_SHIFT)
#define EDATA_BITS_NFREE_MASK  MASK(EDATA_BITS_NFREE_WIDTH, EDATA_BITS_NFREE_SHIFT)

#define EDATA_BITS_BINSHARD_WIDTH  6
#define EDATA_BITS_BINSHARD_SHIFT  (EDATA_BITS_NFREE_WIDTH + EDATA_BITS_NFREE_SHIFT)
#define EDATA_BITS_BINSHARD_MASK  MASK(EDATA_BITS_BINSHARD_WIDTH, EDATA_BITS_BINSHARD_SHIFT)

#define EDATA_BITS_IS_HEAD_WIDTH 1
#define EDATA_BITS_IS_HEAD_SHIFT  (EDATA_BITS_BINSHARD_WIDTH + EDATA_BITS_BINSHARD_SHIFT)
#define EDATA_BITS_IS_HEAD_MASK  MASK(EDATA_BITS_IS_HEAD_WIDTH, EDATA_BITS_IS_HEAD_SHIFT)

#define EDATA_BITS_SN_SHIFT   (EDATA_BITS_IS_HEAD_WIDTH + EDATA_BITS_IS_HEAD_SHIFT)
#define EDATA_BITS_SN_MASK  (UINT64_MAX << EDATA_BITS_SN_SHIFT)

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
	#define EDATA_SIZE_MASK	((size_t)~(PAGE-1))
	#define EDATA_ESN_MASK		((size_t)PAGE-1)
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
	ql_elm(edata_t) ql_link;

	/*
	 * Linkage for per size class sn/address-ordered heaps, and
	 * for extent_avail
	 */
	phn(edata_t)		ph_link;

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
edata_arena_ind_get(const edata_t *edata) {
	unsigned arena_ind = (unsigned)((edata->e_bits &
	    EDATA_BITS_ARENA_MASK) >> EDATA_BITS_ARENA_SHIFT);
	assert(arena_ind < MALLOCX_ARENA_LIMIT);

	return arena_ind;
}

static inline szind_t
edata_szind_get_maybe_invalid(const edata_t *edata) {
	szind_t szind = (szind_t)((edata->e_bits & EDATA_BITS_SZIND_MASK) >>
	    EDATA_BITS_SZIND_SHIFT);
	assert(szind <= SC_NSIZES);
	return szind;
}

static inline szind_t
edata_szind_get(const edata_t *edata) {
	szind_t szind = edata_szind_get_maybe_invalid(edata);
	assert(szind < SC_NSIZES); /* Never call when "invalid". */
	return szind;
}

static inline size_t
edata_usize_get(const edata_t *edata) {
	return sz_index2size(edata_szind_get(edata));
}

static inline unsigned
edata_binshard_get(const edata_t *edata) {
	unsigned binshard = (unsigned)((edata->e_bits &
	    EDATA_BITS_BINSHARD_MASK) >> EDATA_BITS_BINSHARD_SHIFT);
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	return binshard;
}

static inline size_t
edata_sn_get(const edata_t *edata) {
	return (size_t)((edata->e_bits & EDATA_BITS_SN_MASK) >>
	    EDATA_BITS_SN_SHIFT);
}

static inline extent_state_t
edata_state_get(const edata_t *edata) {
	return (extent_state_t)((edata->e_bits & EDATA_BITS_STATE_MASK) >>
	    EDATA_BITS_STATE_SHIFT);
}

static inline bool
edata_zeroed_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_ZEROED_MASK) >>
	    EDATA_BITS_ZEROED_SHIFT);
}

static inline bool
edata_committed_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_COMMITTED_MASK) >>
	    EDATA_BITS_COMMITTED_SHIFT);
}

static inline bool
edata_dumpable_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_DUMPABLE_MASK) >>
	    EDATA_BITS_DUMPABLE_SHIFT);
}

static inline bool
edata_slab_get(const edata_t *edata) {
	return (bool)((edata->e_bits & EDATA_BITS_SLAB_MASK) >>
	    EDATA_BITS_SLAB_SHIFT);
}

static inline unsigned
edata_nfree_get(const edata_t *edata) {
	assert(edata_slab_get(edata));
	return (unsigned)((edata->e_bits & EDATA_BITS_NFREE_MASK) >>
	    EDATA_BITS_NFREE_SHIFT);
}

static inline void *
edata_base_get(const edata_t *edata) {
	assert(edata->e_addr == PAGE_ADDR2BASE(edata->e_addr) ||
	    !edata_slab_get(edata));
	return PAGE_ADDR2BASE(edata->e_addr);
}

static inline void *
edata_addr_get(const edata_t *edata) {
	assert(edata->e_addr == PAGE_ADDR2BASE(edata->e_addr) ||
	    !edata_slab_get(edata));
	return edata->e_addr;
}

static inline size_t
edata_size_get(const edata_t *edata) {
	return (edata->e_size_esn & EDATA_SIZE_MASK);
}

static inline size_t
edata_esn_get(const edata_t *edata) {
	return (edata->e_size_esn & EDATA_ESN_MASK);
}

static inline size_t
edata_bsize_get(const edata_t *edata) {
	return edata->e_bsize;
}

static inline void *
edata_before_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) - PAGE);
}

static inline void *
edata_last_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) +
	    edata_size_get(edata) - PAGE);
}

static inline void *
edata_past_get(const edata_t *edata) {
	return (void *)((uintptr_t)edata_base_get(edata) +
	    edata_size_get(edata));
}

static inline slab_data_t *
edata_slab_data_get(edata_t *edata) {
	assert(edata_slab_get(edata));
	return &edata->e_slab_data;
}

static inline const slab_data_t *
edata_slab_data_get_const(const edata_t *edata) {
	assert(edata_slab_get(edata));
	return &edata->e_slab_data;
}

static inline void
edata_prof_info_get(const edata_t *edata, prof_info_t *prof_info) {
	assert(prof_info != NULL);
	prof_info->alloc_tctx = (prof_tctx_t *)atomic_load_p(
	    &edata->e_prof_tctx, ATOMIC_ACQUIRE);
	prof_info->alloc_time = edata->e_alloc_time;
}

static inline void
edata_arena_ind_set(edata_t *edata, unsigned arena_ind) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_ARENA_MASK) |
	    ((uint64_t)arena_ind << EDATA_BITS_ARENA_SHIFT);
}

static inline void
edata_binshard_set(edata_t *edata, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_BINSHARD_MASK) |
	    ((uint64_t)binshard << EDATA_BITS_BINSHARD_SHIFT);
}

static inline void
edata_addr_set(edata_t *edata, void *addr) {
	edata->e_addr = addr;
}

static inline void
edata_size_set(edata_t *edata, size_t size) {
	assert((size & ~EDATA_SIZE_MASK) == 0);
	edata->e_size_esn = size | (edata->e_size_esn & ~EDATA_SIZE_MASK);
}

static inline void
edata_esn_set(edata_t *edata, size_t esn) {
	edata->e_size_esn = (edata->e_size_esn & ~EDATA_ESN_MASK) | (esn &
	    EDATA_ESN_MASK);
}

static inline void
edata_bsize_set(edata_t *edata, size_t bsize) {
	edata->e_bsize = bsize;
}

static inline void
edata_szind_set(edata_t *edata, szind_t szind) {
	assert(szind <= SC_NSIZES); /* SC_NSIZES means "invalid". */
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_SZIND_MASK) |
	    ((uint64_t)szind << EDATA_BITS_SZIND_SHIFT);
}

static inline void
edata_nfree_set(edata_t *edata, unsigned nfree) {
	assert(edata_slab_get(edata));
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_NFREE_MASK) |
	    ((uint64_t)nfree << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_binshard_set(edata_t *edata, unsigned nfree, unsigned binshard) {
	/* The assertion assumes szind is set already. */
	assert(binshard < bin_infos[edata_szind_get(edata)].n_shards);
	edata->e_bits = (edata->e_bits &
	    (~EDATA_BITS_NFREE_MASK & ~EDATA_BITS_BINSHARD_MASK)) |
	    ((uint64_t)binshard << EDATA_BITS_BINSHARD_SHIFT) |
	    ((uint64_t)nfree << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_inc(edata_t *edata) {
	assert(edata_slab_get(edata));
	edata->e_bits += ((uint64_t)1U << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_dec(edata_t *edata) {
	assert(edata_slab_get(edata));
	edata->e_bits -= ((uint64_t)1U << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_nfree_sub(edata_t *edata, uint64_t n) {
	assert(edata_slab_get(edata));
	edata->e_bits -= (n << EDATA_BITS_NFREE_SHIFT);
}

static inline void
edata_sn_set(edata_t *edata, size_t sn) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_SN_MASK) |
	    ((uint64_t)sn << EDATA_BITS_SN_SHIFT);
}

static inline void
edata_state_set(edata_t *edata, extent_state_t state) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_STATE_MASK) |
	    ((uint64_t)state << EDATA_BITS_STATE_SHIFT);
}

static inline void
edata_zeroed_set(edata_t *edata, bool zeroed) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_ZEROED_MASK) |
	    ((uint64_t)zeroed << EDATA_BITS_ZEROED_SHIFT);
}

static inline void
edata_committed_set(edata_t *edata, bool committed) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_COMMITTED_MASK) |
	    ((uint64_t)committed << EDATA_BITS_COMMITTED_SHIFT);
}

static inline void
edata_dumpable_set(edata_t *edata, bool dumpable) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_DUMPABLE_MASK) |
	    ((uint64_t)dumpable << EDATA_BITS_DUMPABLE_SHIFT);
}

static inline void
edata_slab_set(edata_t *edata, bool slab) {
	edata->e_bits = (edata->e_bits & ~EDATA_BITS_SLAB_MASK) |
	    ((uint64_t)slab << EDATA_BITS_SLAB_SHIFT);
}

static inline void
edata_prof_tctx_set(edata_t *edata, prof_tctx_t *tctx) {
	atomic_store_p(&edata->e_prof_tctx, tctx, ATOMIC_RELEASE);
}

static inline void
edata_prof_alloc_time_set(edata_t *edata, nstime_t *t) {
	nstime_copy(&edata->e_alloc_time, t);
}

static inline bool
edata_is_head_get(edata_t *edata) {
	if (maps_coalesce) {
		not_reached();
	}

	return (bool)((edata->e_bits & EDATA_BITS_IS_HEAD_MASK) >>
	    EDATA_BITS_IS_HEAD_SHIFT);
}

static inline void
edata_is_head_set(edata_t *edata, bool is_head) {
	if (maps_coalesce) {
		not_reached();
	}

	edata->e_bits = (edata->e_bits & ~EDATA_BITS_IS_HEAD_MASK) |
	    ((uint64_t)is_head << EDATA_BITS_IS_HEAD_SHIFT);
}

static inline void
edata_init(edata_t *edata, unsigned arena_ind, void *addr, size_t size,
    bool slab, szind_t szind, size_t sn, extent_state_t state, bool zeroed,
    bool committed, bool dumpable, extent_head_state_t is_head) {
	assert(addr == PAGE_ADDR2BASE(addr) || !slab);

	edata_arena_ind_set(edata, arena_ind);
	edata_addr_set(edata, addr);
	edata_size_set(edata, size);
	edata_slab_set(edata, slab);
	edata_szind_set(edata, szind);
	edata_sn_set(edata, sn);
	edata_state_set(edata, state);
	edata_zeroed_set(edata, zeroed);
	edata_committed_set(edata, committed);
	edata_dumpable_set(edata, dumpable);
	ql_elm_new(edata, ql_link);
	if (!maps_coalesce) {
		edata_is_head_set(edata, is_head == EXTENT_IS_HEAD);
	}
	if (config_prof) {
		edata_prof_tctx_set(edata, NULL);
	}
}

static inline void
edata_binit(edata_t *edata, void *addr, size_t bsize, size_t sn) {
	edata_arena_ind_set(edata, (1U << MALLOCX_ARENA_BITS) - 1);
	edata_addr_set(edata, addr);
	edata_bsize_set(edata, bsize);
	edata_slab_set(edata, false);
	edata_szind_set(edata, SC_NSIZES);
	edata_sn_set(edata, sn);
	edata_state_set(edata, extent_state_active);
	edata_zeroed_set(edata, true);
	edata_committed_set(edata, true);
	edata_dumpable_set(edata, true);
}

static inline void
edata_list_init(edata_list_t *list) {
	ql_new(list);
}

static inline edata_t *
edata_list_first(const edata_list_t *list) {
	return ql_first(list);
}

static inline edata_t *
edata_list_last(const edata_list_t *list) {
	return ql_last(list, ql_link);
}

static inline void
edata_list_append(edata_list_t *list, edata_t *edata) {
	ql_tail_insert(list, edata, ql_link);
}

static inline void
edata_list_prepend(edata_list_t *list, edata_t *edata) {
	ql_head_insert(list, edata, ql_link);
}

static inline void
edata_list_replace(edata_list_t *list, edata_t *to_remove,
    edata_t *to_insert) {
	ql_after_insert(to_remove, to_insert, ql_link);
	ql_remove(list, to_remove, ql_link);
}

static inline void
edata_list_remove(edata_list_t *list, edata_t *edata) {
	ql_remove(list, edata, ql_link);
}

static inline int
edata_sn_comp(const edata_t *a, const edata_t *b) {
	size_t a_sn = edata_sn_get(a);
	size_t b_sn = edata_sn_get(b);

	return (a_sn > b_sn) - (a_sn < b_sn);
}

static inline int
edata_esn_comp(const edata_t *a, const edata_t *b) {
	size_t a_esn = edata_esn_get(a);
	size_t b_esn = edata_esn_get(b);

	return (a_esn > b_esn) - (a_esn < b_esn);
}

static inline int
edata_ad_comp(const edata_t *a, const edata_t *b) {
	uintptr_t a_addr = (uintptr_t)edata_addr_get(a);
	uintptr_t b_addr = (uintptr_t)edata_addr_get(b);

	return (a_addr > b_addr) - (a_addr < b_addr);
}

static inline int
edata_ead_comp(const edata_t *a, const edata_t *b) {
	uintptr_t a_eaddr = (uintptr_t)a;
	uintptr_t b_eaddr = (uintptr_t)b;

	return (a_eaddr > b_eaddr) - (a_eaddr < b_eaddr);
}

static inline int
edata_snad_comp(const edata_t *a, const edata_t *b) {
	int ret;

	ret = edata_sn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = edata_ad_comp(a, b);
	return ret;
}

static inline int
edata_esnead_comp(const edata_t *a, const edata_t *b) {
	int ret;

	ret = edata_esn_comp(a, b);
	if (ret != 0) {
		return ret;
	}

	ret = edata_ead_comp(a, b);
	return ret;
}

ph_proto(, edata_avail_, edata_tree_t, edata_t)
ph_proto(, edata_heap_, edata_heap_t, edata_t)

#endif /* JEMALLOC_INTERNAL_EDATA_H */
