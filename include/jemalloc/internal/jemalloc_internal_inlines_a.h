#ifndef JEMALLOC_INTERNAL_INLINES_A_H
#define JEMALLOC_INTERNAL_INLINES_A_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/size_classes.h"
#include "jemalloc/internal/ticker.h"

JEMALLOC_ALWAYS_INLINE pszind_t
psz2ind(size_t psz) {
	if (unlikely(psz > LARGE_MAXCLASS)) {
		return NPSIZES;
	}
	{
		pszind_t x = lg_floor((psz<<1)-1);
		pszind_t shift = (x < LG_SIZE_CLASS_GROUP + LG_PAGE) ? 0 : x -
		    (LG_SIZE_CLASS_GROUP + LG_PAGE);
		pszind_t grp = shift << LG_SIZE_CLASS_GROUP;

		pszind_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_PAGE + 1) ?
		    LG_PAGE : x - LG_SIZE_CLASS_GROUP - 1;

		size_t delta_inverse_mask = ZD(-1) << lg_delta;
		pszind_t mod = ((((psz-1) & delta_inverse_mask) >> lg_delta)) &
		    ((ZU(1) << LG_SIZE_CLASS_GROUP) - 1);

		pszind_t ind = grp + mod;
		return ind;
	}
}

static inline size_t
pind2sz_compute(pszind_t pind) {
	if (unlikely(pind == NPSIZES)) {
		return LARGE_MAXCLASS + PAGE;
	}
	{
		size_t grp = pind >> LG_SIZE_CLASS_GROUP;
		size_t mod = pind & ((ZU(1) << LG_SIZE_CLASS_GROUP) - 1);

		size_t grp_size_mask = ~((!!grp)-1);
		size_t grp_size = ((ZU(1) << (LG_PAGE +
		    (LG_SIZE_CLASS_GROUP-1))) << grp) & grp_size_mask;

		size_t shift = (grp == 0) ? 1 : grp;
		size_t lg_delta = shift + (LG_PAGE-1);
		size_t mod_size = (mod+1) << lg_delta;

		size_t sz = grp_size + mod_size;
		return sz;
	}
}

static inline size_t
pind2sz_lookup(pszind_t pind) {
	size_t ret = (size_t)pind2sz_tab[pind];
	assert(ret == pind2sz_compute(pind));
	return ret;
}

static inline size_t
pind2sz(pszind_t pind) {
	assert(pind < NPSIZES+1);
	return pind2sz_lookup(pind);
}

static inline size_t
psz2u(size_t psz) {
	if (unlikely(psz > LARGE_MAXCLASS)) {
		return LARGE_MAXCLASS + PAGE;
	}
	{
		size_t x = lg_floor((psz<<1)-1);
		size_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_PAGE + 1) ?
		    LG_PAGE : x - LG_SIZE_CLASS_GROUP - 1;
		size_t delta = ZU(1) << lg_delta;
		size_t delta_mask = delta - 1;
		size_t usize = (psz + delta_mask) & ~delta_mask;
		return usize;
	}
}

static inline szind_t
size2index_compute(size_t size) {
	if (unlikely(size > LARGE_MAXCLASS)) {
		return NSIZES;
	}
#if (NTBINS != 0)
	if (size <= (ZU(1) << LG_TINY_MAXCLASS)) {
		szind_t lg_tmin = LG_TINY_MAXCLASS - NTBINS + 1;
		szind_t lg_ceil = lg_floor(pow2_ceil_zu(size));
		return (lg_ceil < lg_tmin ? 0 : lg_ceil - lg_tmin);
	}
#endif
	{
		szind_t x = lg_floor((size<<1)-1);
		szind_t shift = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM) ? 0 :
		    x - (LG_SIZE_CLASS_GROUP + LG_QUANTUM);
		szind_t grp = shift << LG_SIZE_CLASS_GROUP;

		szind_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM + 1)
		    ? LG_QUANTUM : x - LG_SIZE_CLASS_GROUP - 1;

		size_t delta_inverse_mask = ZD(-1) << lg_delta;
		szind_t mod = ((((size-1) & delta_inverse_mask) >> lg_delta)) &
		    ((ZU(1) << LG_SIZE_CLASS_GROUP) - 1);

		szind_t index = NTBINS + grp + mod;
		return index;
	}
}

JEMALLOC_ALWAYS_INLINE szind_t
size2index_lookup(size_t size) {
	assert(size <= LOOKUP_MAXCLASS);
	{
		szind_t ret = (size2index_tab[(size-1) >> LG_TINY_MIN]);
		assert(ret == size2index_compute(size));
		return ret;
	}
}

JEMALLOC_ALWAYS_INLINE szind_t
size2index(size_t size) {
	assert(size > 0);
	if (likely(size <= LOOKUP_MAXCLASS)) {
		return size2index_lookup(size);
	}
	return size2index_compute(size);
}

static inline size_t
index2size_compute(szind_t index) {
#if (NTBINS > 0)
	if (index < NTBINS) {
		return (ZU(1) << (LG_TINY_MAXCLASS - NTBINS + 1 + index));
	}
#endif
	{
		size_t reduced_index = index - NTBINS;
		size_t grp = reduced_index >> LG_SIZE_CLASS_GROUP;
		size_t mod = reduced_index & ((ZU(1) << LG_SIZE_CLASS_GROUP) -
		    1);

		size_t grp_size_mask = ~((!!grp)-1);
		size_t grp_size = ((ZU(1) << (LG_QUANTUM +
		    (LG_SIZE_CLASS_GROUP-1))) << grp) & grp_size_mask;

		size_t shift = (grp == 0) ? 1 : grp;
		size_t lg_delta = shift + (LG_QUANTUM-1);
		size_t mod_size = (mod+1) << lg_delta;

		size_t usize = grp_size + mod_size;
		return usize;
	}
}

JEMALLOC_ALWAYS_INLINE size_t
index2size_lookup(szind_t index) {
	size_t ret = (size_t)index2size_tab[index];
	assert(ret == index2size_compute(index));
	return ret;
}

JEMALLOC_ALWAYS_INLINE size_t
index2size(szind_t index) {
	assert(index < NSIZES);
	return index2size_lookup(index);
}

JEMALLOC_ALWAYS_INLINE size_t
s2u_compute(size_t size) {
	if (unlikely(size > LARGE_MAXCLASS)) {
		return 0;
	}
#if (NTBINS > 0)
	if (size <= (ZU(1) << LG_TINY_MAXCLASS)) {
		size_t lg_tmin = LG_TINY_MAXCLASS - NTBINS + 1;
		size_t lg_ceil = lg_floor(pow2_ceil_zu(size));
		return (lg_ceil < lg_tmin ? (ZU(1) << lg_tmin) :
		    (ZU(1) << lg_ceil));
	}
#endif
	{
		size_t x = lg_floor((size<<1)-1);
		size_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM + 1)
		    ?  LG_QUANTUM : x - LG_SIZE_CLASS_GROUP - 1;
		size_t delta = ZU(1) << lg_delta;
		size_t delta_mask = delta - 1;
		size_t usize = (size + delta_mask) & ~delta_mask;
		return usize;
	}
}

JEMALLOC_ALWAYS_INLINE size_t
s2u_lookup(size_t size) {
	size_t ret = index2size_lookup(size2index_lookup(size));

	assert(ret == s2u_compute(size));
	return ret;
}

/*
 * Compute usable size that would result from allocating an object with the
 * specified size.
 */
JEMALLOC_ALWAYS_INLINE size_t
s2u(size_t size) {
	assert(size > 0);
	if (likely(size <= LOOKUP_MAXCLASS)) {
		return s2u_lookup(size);
	}
	return s2u_compute(size);
}

/*
 * Compute usable size that would result from allocating an object with the
 * specified size and alignment.
 */
JEMALLOC_ALWAYS_INLINE size_t
sa2u(size_t size, size_t alignment) {
	size_t usize;

	assert(alignment != 0 && ((alignment - 1) & alignment) == 0);

	/* Try for a small size class. */
	if (size <= SMALL_MAXCLASS && alignment < PAGE) {
		/*
		 * Round size up to the nearest multiple of alignment.
		 *
		 * This done, we can take advantage of the fact that for each
		 * small size class, every object is aligned at the smallest
		 * power of two that is non-zero in the base two representation
		 * of the size.  For example:
		 *
		 *   Size |   Base 2 | Minimum alignment
		 *   -----+----------+------------------
		 *     96 |  1100000 |  32
		 *    144 | 10100000 |  32
		 *    192 | 11000000 |  64
		 */
		usize = s2u(ALIGNMENT_CEILING(size, alignment));
		if (usize < LARGE_MINCLASS) {
			return usize;
		}
	}

	/* Large size class.  Beware of overflow. */

	if (unlikely(alignment > LARGE_MAXCLASS)) {
		return 0;
	}

	/* Make sure result is a large size class. */
	if (size <= LARGE_MINCLASS) {
		usize = LARGE_MINCLASS;
	} else {
		usize = s2u(size);
		if (usize < size) {
			/* size_t overflow. */
			return 0;
		}
	}

	/*
	 * Calculate the multi-page mapping that large_palloc() would need in
	 * order to guarantee the alignment.
	 */
	if (usize + large_pad + PAGE_CEILING(alignment) - PAGE < usize) {
		/* size_t overflow. */
		return 0;
	}
	return usize;
}

JEMALLOC_ALWAYS_INLINE malloc_cpuid_t
malloc_getcpu(void) {
	assert(have_percpu_arena);
#if defined(JEMALLOC_HAVE_SCHED_GETCPU)
	return (malloc_cpuid_t)sched_getcpu();
#else
	not_reached();
	return -1;
#endif
}

/* Return the chosen arena index based on current cpu. */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_choose(void) {
	unsigned arena_ind;
	assert(have_percpu_arena && (percpu_arena_mode !=
	    percpu_arena_disabled));

	malloc_cpuid_t cpuid = malloc_getcpu();
	assert(cpuid >= 0);
	if ((percpu_arena_mode == percpu_arena) ||
	    ((unsigned)cpuid < ncpus / 2)) {
		arena_ind = cpuid;
	} else {
		assert(percpu_arena_mode == per_phycpu_arena);
		/* Hyper threads on the same physical CPU share arena. */
		arena_ind = cpuid - ncpus / 2;
	}

	return arena_ind;
}

/* Return the limit of percpu auto arena range, i.e. arenas[0...ind_limit). */
JEMALLOC_ALWAYS_INLINE unsigned
percpu_arena_ind_limit(void) {
	assert(have_percpu_arena && (percpu_arena_mode != percpu_arena_disabled));
	if (percpu_arena_mode == per_phycpu_arena && ncpus > 1) {
		if (ncpus % 2) {
			/* This likely means a misconfig. */
			return ncpus / 2 + 1;
		}
		return ncpus / 2;
	} else {
		return ncpus;
	}
}

static inline arena_tdata_t *
arena_tdata_get(tsd_t *tsd, unsigned ind, bool refresh_if_missing) {
	arena_tdata_t *tdata;
	arena_tdata_t *arenas_tdata = tsd_arenas_tdata_get(tsd);

	if (unlikely(arenas_tdata == NULL)) {
		/* arenas_tdata hasn't been initialized yet. */
		return arena_tdata_get_hard(tsd, ind);
	}
	if (unlikely(ind >= tsd_narenas_tdata_get(tsd))) {
		/*
		 * ind is invalid, cache is old (too small), or tdata to be
		 * initialized.
		 */
		return (refresh_if_missing ? arena_tdata_get_hard(tsd, ind) :
		    NULL);
	}

	tdata = &arenas_tdata[ind];
	if (likely(tdata != NULL) || !refresh_if_missing) {
		return tdata;
	}
	return arena_tdata_get_hard(tsd, ind);
}

static inline arena_t *
arena_get(tsdn_t *tsdn, unsigned ind, bool init_if_missing) {
	arena_t *ret;

	assert(ind < MALLOCX_ARENA_LIMIT);

	ret = (arena_t *)atomic_load_p(&arenas[ind], ATOMIC_ACQUIRE);
	if (unlikely(ret == NULL)) {
		if (init_if_missing) {
			ret = arena_init(tsdn, ind,
			    (extent_hooks_t *)&extent_hooks_default);
		}
	}
	return ret;
}

static inline ticker_t *
decay_ticker_get(tsd_t *tsd, unsigned ind) {
	arena_tdata_t *tdata;

	tdata = arena_tdata_get(tsd, ind, true);
	if (unlikely(tdata == NULL)) {
		return NULL;
	}
	return &tdata->decay_ticker;
}

JEMALLOC_ALWAYS_INLINE tcache_bin_t *
tcache_small_bin_get(tcache_t *tcache, szind_t binind) {
	assert(binind < NBINS);
	return &tcache->tbins_small[binind];
}

JEMALLOC_ALWAYS_INLINE tcache_bin_t *
tcache_large_bin_get(tcache_t *tcache, szind_t binind) {
	assert(binind >= NBINS &&binind < nhbins);
	return &tcache->tbins_large[binind - NBINS];
}

JEMALLOC_ALWAYS_INLINE bool
tcache_available(tsd_t *tsd) {
	/*
	 * Thread specific auto tcache might be unavailable if: 1) during tcache
	 * initialization, or 2) disabled through thread.tcache.enabled mallctl
	 * or config options.  This check covers all cases.
	 */
	if (likely(tsd_tcache_enabled_get(tsd))) {
		/* Associated arena == NULL implies tcache init in progress. */
		assert(tsd_tcachep_get(tsd)->arena == NULL ||
		    tcache_small_bin_get(tsd_tcachep_get(tsd), 0)->avail !=
		    NULL);
		return true;
	}

	return false;
}

JEMALLOC_ALWAYS_INLINE tcache_t *
tcache_get(tsd_t *tsd) {
	if (!tcache_available(tsd)) {
		return NULL;
	}

	return tsd_tcachep_get(tsd);
}

static inline void
pre_reentrancy(tsd_t *tsd) {
	bool fast = tsd_fast(tsd);
	++*tsd_reentrancy_levelp_get(tsd);
	if (fast) {
		/* Prepare slow path for reentrancy. */
		tsd_slow_update(tsd);
		assert(tsd->state == tsd_state_nominal_slow);
	}
}

static inline void
post_reentrancy(tsd_t *tsd) {
	int8_t *reentrancy_level = tsd_reentrancy_levelp_get(tsd);
	assert(*reentrancy_level > 0);
	if (--*reentrancy_level == 0) {
		tsd_slow_update(tsd);
	}
}

#endif /* JEMALLOC_INTERNAL_INLINES_A_H */
