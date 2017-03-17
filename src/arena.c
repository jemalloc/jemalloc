#define JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

const char	*percpu_arena_mode_names[] = {
	"disabled",
	"percpu",
	"phycpu"
};

const char	*opt_percpu_arena = OPT_PERCPU_ARENA_DEFAULT;
percpu_arena_mode_t	percpu_arena_mode = PERCPU_ARENA_MODE_DEFAULT;

ssize_t		opt_dirty_decay_time = DIRTY_DECAY_TIME_DEFAULT;
ssize_t		opt_muzzy_decay_time = MUZZY_DECAY_TIME_DEFAULT;
static ssize_t	dirty_decay_time_default;
static ssize_t	muzzy_decay_time_default;

const arena_bin_info_t	arena_bin_info[NBINS] = {
#define BIN_INFO_bin_yes(reg_size, slab_size, nregs)			\
	{reg_size, slab_size, nregs, BITMAP_INFO_INITIALIZER(nregs)},
#define BIN_INFO_bin_no(reg_size, slab_size, nregs)
#define SC(index, lg_grp, lg_delta, ndelta, psz, bin, pgs,		\
    lg_delta_lookup)							\
	BIN_INFO_bin_##bin((1U<<lg_grp) + (ndelta<<lg_delta),		\
	    (pgs << LG_PAGE), (pgs << LG_PAGE) / ((1U<<lg_grp) +	\
	    (ndelta<<lg_delta)))
	SIZE_CLASSES
#undef BIN_INFO_bin_yes
#undef BIN_INFO_bin_no
#undef SC
};

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void arena_decay_to_limit(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, extents_t *extents, bool all, size_t npages_limit);
static void arena_decay_dirty(tsdn_t *tsdn, arena_t *arena, bool all);
static void arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin);
static void arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin);

/******************************************************************************/

static bool
arena_stats_init(tsdn_t *tsdn, arena_stats_t *arena_stats) {
	if (config_debug) {
		for (size_t i = 0; i < sizeof(arena_stats_t); i++) {
			assert(((char *)arena_stats)[i] == 0);
		}
	}
#ifndef JEMALLOC_ATOMIC_U64
	if (malloc_mutex_init(&arena_stats->mtx, "arena_stats",
	    WITNESS_RANK_ARENA_STATS)) {
		return true;
	}
#endif
	/* Memory is zeroed, so there is no need to clear stats. */
	return false;
}

static void
arena_stats_lock(tsdn_t *tsdn, arena_stats_t *arena_stats) {
#ifndef JEMALLOC_ATOMIC_U64
	malloc_mutex_lock(tsdn, &arena_stats->mtx);
#endif
}

static void
arena_stats_unlock(tsdn_t *tsdn, arena_stats_t *arena_stats) {
#ifndef JEMALLOC_ATOMIC_U64
	malloc_mutex_unlock(tsdn, &arena_stats->mtx);
#endif
}

static uint64_t
arena_stats_read_u64(tsdn_t *tsdn, arena_stats_t *arena_stats,
    arena_stats_u64_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_u64(p, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	return *p;
#endif
}

static void
arena_stats_add_u64(tsdn_t *tsdn, arena_stats_t *arena_stats,
    arena_stats_u64_t *p, uint64_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_u64(p, x, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	*p += x;
#endif
}

UNUSED static void
arena_stats_sub_u64(tsdn_t *tsdn, arena_stats_t *arena_stats,
    arena_stats_u64_t *p, uint64_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	UNUSED uint64_t r = atomic_fetch_sub_u64(p, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	*p -= x;
	assert(*p + x >= *p);
#endif
}

/*
 * Non-atomically sets *dst += src.  *dst needs external synchronization.
 * This lets us avoid the cost of a fetch_add when its unnecessary (note that
 * the types here are atomic).
 */
static void
arena_stats_accum_u64(arena_stats_u64_t *dst, uint64_t src) {
#ifdef JEMALLOC_ATOMIC_U64
	uint64_t cur_dst = atomic_load_u64(dst, ATOMIC_RELAXED);
	atomic_store_u64(dst, src + cur_dst, ATOMIC_RELAXED);
#else
	*dst += src;
#endif
}

static size_t
arena_stats_read_zu(tsdn_t *tsdn, arena_stats_t *arena_stats, atomic_zu_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_zu(p, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	return atomic_load_zu(p, ATOMIC_RELAXED);
#endif
}

static void
arena_stats_add_zu(tsdn_t *tsdn, arena_stats_t *arena_stats, atomic_zu_t *p,
    size_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_zu(p, x, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	size_t cur = atomic_load_zu(p, ATOMIC_RELAXED);
	atomic_store_zu(p, cur + x, ATOMIC_RELAXED);
#endif
}

static void
arena_stats_sub_zu(tsdn_t *tsdn, arena_stats_t *arena_stats, atomic_zu_t *p,
    size_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	UNUSED size_t r = atomic_fetch_sub_zu(p, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	malloc_mutex_assert_owner(tsdn, &arena_stats->mtx);
	size_t cur = atomic_load_zu(p, ATOMIC_RELAXED);
	atomic_store_zu(p, cur - x, ATOMIC_RELAXED);
#endif
}

/* Like the _u64 variant, needs an externally synchronized *dst. */
static void
arena_stats_accum_zu(atomic_zu_t *dst, size_t src) {
	size_t cur_dst = atomic_load_zu(dst, ATOMIC_RELAXED);
	atomic_store_zu(dst, src + cur_dst, ATOMIC_RELAXED);
}

void
arena_stats_large_nrequests_add(tsdn_t *tsdn, arena_stats_t *arena_stats,
    szind_t szind, uint64_t nrequests) {
	arena_stats_lock(tsdn, arena_stats);
	arena_stats_add_u64(tsdn, arena_stats, &arena_stats->lstats[szind -
	    NBINS].nrequests, nrequests);
	arena_stats_unlock(tsdn, arena_stats);
}

void
arena_stats_mapped_add(tsdn_t *tsdn, arena_stats_t *arena_stats, size_t size) {
	arena_stats_lock(tsdn, arena_stats);
	arena_stats_add_zu(tsdn, arena_stats, &arena_stats->mapped, size);
	arena_stats_unlock(tsdn, arena_stats);
}

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_time, ssize_t *muzzy_decay_time,
    size_t *nactive, size_t *ndirty,
    size_t *nmuzzy) {
	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena_dss_prec_get(arena)];
	*dirty_decay_time = arena_dirty_decay_time_get(arena);
	*muzzy_decay_time = arena_muzzy_decay_time_get(arena);
	*nactive += atomic_read_zu(&arena->nactive);
	*ndirty += extents_npages_get(&arena->extents_dirty);
	*nmuzzy += extents_npages_get(&arena->extents_muzzy);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_time, ssize_t *muzzy_decay_time,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    malloc_bin_stats_t *bstats, malloc_large_stats_t *lstats) {
	cassert(config_stats);

	arena_basic_stats_merge(tsdn, arena, nthreads, dss, dirty_decay_time,
	    muzzy_decay_time, nactive, ndirty, nmuzzy);

	size_t base_allocated, base_resident, base_mapped;
	base_stats_get(tsdn, arena->base, &base_allocated, &base_resident,
	    &base_mapped);

	arena_stats_lock(tsdn, &arena->stats);

	arena_stats_accum_zu(&astats->mapped, base_mapped
	    + arena_stats_read_zu(tsdn, &arena->stats, &arena->stats.mapped));
	arena_stats_accum_zu(&astats->retained,
	    extents_npages_get(&arena->extents_retained) << LG_PAGE);

	arena_stats_accum_u64(&astats->decay_dirty.npurge,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.npurge));
	arena_stats_accum_u64(&astats->decay_dirty.nmadvise,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.nmadvise));
	arena_stats_accum_u64(&astats->decay_dirty.purged,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_dirty.purged));

	arena_stats_accum_u64(&astats->decay_muzzy.npurge,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.npurge));
	arena_stats_accum_u64(&astats->decay_muzzy.nmadvise,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.nmadvise));
	arena_stats_accum_u64(&astats->decay_muzzy.purged,
	    arena_stats_read_u64(tsdn, &arena->stats,
	    &arena->stats.decay_muzzy.purged));

	arena_stats_accum_zu(&astats->base, base_allocated);
	arena_stats_accum_zu(&astats->internal, arena_internal_get(arena));
	arena_stats_accum_zu(&astats->resident, base_resident +
	    (((atomic_read_zu(&arena->nactive) +
	    extents_npages_get(&arena->extents_dirty) +
	    extents_npages_get(&arena->extents_muzzy)) << LG_PAGE)));

	for (szind_t i = 0; i < NSIZES - NBINS; i++) {
		uint64_t nmalloc = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].nmalloc);
		arena_stats_accum_u64(&lstats[i].nmalloc, nmalloc);
		arena_stats_accum_u64(&astats->nmalloc_large, nmalloc);

		uint64_t ndalloc = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].ndalloc);
		arena_stats_accum_u64(&lstats[i].ndalloc, ndalloc);
		arena_stats_accum_u64(&astats->ndalloc_large, ndalloc);

		uint64_t nrequests = arena_stats_read_u64(tsdn, &arena->stats,
		    &arena->stats.lstats[i].nrequests);
		arena_stats_accum_u64(&lstats[i].nrequests,
		    nmalloc + nrequests);
		arena_stats_accum_u64(&astats->nrequests_large,
		    nmalloc + nrequests);

		assert(nmalloc >= ndalloc);
		assert(nmalloc - ndalloc <= SIZE_T_MAX);
		size_t curlextents = (size_t)(nmalloc - ndalloc);
		lstats[i].curlextents += curlextents;
		arena_stats_accum_zu(&astats->allocated_large,
		    curlextents * index2size(NBINS + i));
	}

	arena_stats_unlock(tsdn, &arena->stats);

	if (config_tcache) {
		tcache_bin_t *tbin;
		tcache_t *tcache;

		/* tcache_bytes counts currently cached bytes. */
		atomic_store_zu(&astats->tcache_bytes, 0, ATOMIC_RELAXED);
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		ql_foreach(tcache, &arena->tcache_ql, link) {
			for (szind_t i = 0; i < nhbins; i++) {
				tbin = &tcache->tbins[i];
				arena_stats_accum_zu(&astats->tcache_bytes,
				    tbin->ncached * index2size(i));
			}
		}
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}

	for (szind_t i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];

		malloc_mutex_lock(tsdn, &bin->lock);
		bstats[i].nmalloc += bin->stats.nmalloc;
		bstats[i].ndalloc += bin->stats.ndalloc;
		bstats[i].nrequests += bin->stats.nrequests;
		bstats[i].curregs += bin->stats.curregs;
		if (config_tcache) {
			bstats[i].nfills += bin->stats.nfills;
			bstats[i].nflushes += bin->stats.nflushes;
		}
		bstats[i].nslabs += bin->stats.nslabs;
		bstats[i].reslabs += bin->stats.reslabs;
		bstats[i].curslabs += bin->stats.curslabs;
		malloc_mutex_unlock(tsdn, &bin->lock);
	}
}

void
arena_extents_dirty_dalloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extents_dalloc(tsdn, arena, r_extent_hooks, &arena->extents_dirty,
	    extent);
	if (arena_dirty_decay_time_get(arena) == 0) {
		arena_decay_dirty(tsdn, arena, true);
	}
}

JEMALLOC_INLINE_C void *
arena_slab_reg_alloc(tsdn_t *tsdn, extent_t *slab,
    const arena_bin_info_t *bin_info) {
	void *ret;
	arena_slab_data_t *slab_data = extent_slab_data_get(slab);
	size_t regind;

	assert(slab_data->nfree > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)extent_addr_get(slab) +
	    (uintptr_t)(bin_info->reg_size * regind));
	slab_data->nfree--;
	return ret;
}

#ifndef JEMALLOC_JET
JEMALLOC_INLINE_C
#endif
size_t
arena_slab_regind(extent_t *slab, szind_t binind, const void *ptr) {
	size_t diff, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)extent_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)extent_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab)) %
	    (uintptr_t)arena_bin_info[binind].reg_size == 0);

	/* Avoid doing division with a variable divisor. */
	diff = (size_t)((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab));
	switch (binind) {
#define REGIND_bin_yes(index, reg_size)					\
	case index:							\
		regind = diff / (reg_size);				\
		assert(diff == regind * (reg_size));			\
		break;
#define REGIND_bin_no(index, reg_size)
#define SC(index, lg_grp, lg_delta, ndelta, psz, bin, pgs,		\
    lg_delta_lookup)							\
	REGIND_bin_##bin(index, (1U<<lg_grp) + (ndelta<<lg_delta))
	SIZE_CLASSES
#undef REGIND_bin_yes
#undef REGIND_bin_no
#undef SC
	default: not_reached();
	}

	assert(regind < arena_bin_info[binind].nregs);

	return regind;
}

JEMALLOC_INLINE_C void
arena_slab_reg_dalloc(tsdn_t *tsdn, extent_t *slab,
    arena_slab_data_t *slab_data, void *ptr) {
	szind_t binind = extent_szind_get(slab);
	const arena_bin_info_t *bin_info = &arena_bin_info[binind];
	size_t regind = arena_slab_regind(slab, binind, ptr);

	assert(slab_data->nfree < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	slab_data->nfree++;
}

static void
arena_nactive_add(arena_t *arena, size_t add_pages) {
	atomic_add_zu(&arena->nactive, add_pages);
}

static void
arena_nactive_sub(arena_t *arena, size_t sub_pages) {
	assert(atomic_read_zu(&arena->nactive) >= sub_pages);
	atomic_sub_zu(&arena->nactive, sub_pages);
}

static void
arena_large_malloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < LARGE_MINCLASS) {
		usize = LARGE_MINCLASS;
	}
	index = size2index(usize);
	hindex = (index >= NBINS) ? index - NBINS : 0;

	arena_stats_add_u64(tsdn, &arena->stats,
	    &arena->stats.lstats[hindex].nmalloc, 1);
}

static void
arena_large_dalloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t usize) {
	szind_t index, hindex;

	cassert(config_stats);

	if (usize < LARGE_MINCLASS) {
		usize = LARGE_MINCLASS;
	}
	index = size2index(usize);
	hindex = (index >= NBINS) ? index - NBINS : 0;

	arena_stats_add_u64(tsdn, &arena->stats,
	    &arena->stats.lstats[hindex].ndalloc, 1);
}

static void
arena_large_ralloc_stats_update(tsdn_t *tsdn, arena_t *arena, size_t oldusize,
    size_t usize) {
	arena_large_dalloc_stats_update(tsdn, arena, oldusize);
	arena_large_malloc_stats_update(tsdn, arena, usize);
}

extent_t *
arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool *zero) {
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	szind_t szind = size2index(usize);
	size_t mapped_add;
	bool commit = true;
	extent_t *extent = extents_alloc(tsdn, arena, &extent_hooks,
	    &arena->extents_dirty, NULL, usize, large_pad, alignment, false,
	    szind, zero, &commit);
	if (extent == NULL) {
		extent = extents_alloc(tsdn, arena, &extent_hooks,
		    &arena->extents_muzzy, NULL, usize, large_pad, alignment,
		    false, szind, zero, &commit);
	}
	size_t size = usize + large_pad;
	if (extent == NULL) {
		extent = extent_alloc_wrapper(tsdn, arena, &extent_hooks, NULL,
		    usize, large_pad, alignment, false, szind, zero, &commit);
		if (config_stats) {
			/*
			 * extent may be NULL on OOM, but in that case
			 * mapped_add isn't used below, so there's no need to
			 * conditionlly set it to 0 here.
			 */
			mapped_add = size;
		}
	} else if (config_stats) {
		mapped_add = 0;
	}

	if (extent != NULL) {
		if (config_stats) {
			arena_stats_lock(tsdn, &arena->stats);
			arena_large_malloc_stats_update(tsdn, arena, usize);
			if (mapped_add != 0) {
				arena_stats_add_zu(tsdn, &arena->stats,
				    &arena->stats.mapped, mapped_add);
			}
			arena_stats_unlock(tsdn, &arena->stats);
		}
		arena_nactive_add(arena, size >> LG_PAGE);
	}

	return extent;
}

void
arena_extent_dalloc_large_prep(tsdn_t *tsdn, arena_t *arena, extent_t *extent) {
	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_dalloc_stats_update(tsdn, arena,
		    extent_usize_get(extent));
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_sub(arena, extent_size_get(extent) >> LG_PAGE);
}

void
arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize) {
	size_t usize = extent_usize_get(extent);
	size_t udiff = oldusize - usize;

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_sub(arena, udiff >> LG_PAGE);
}

void
arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize) {
	size_t usize = extent_usize_get(extent);
	size_t udiff = usize - oldusize;

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_large_ralloc_stats_update(tsdn, arena, oldusize, usize);
		arena_stats_unlock(tsdn, &arena->stats);
	}
	arena_nactive_add(arena, udiff >> LG_PAGE);
}

static ssize_t
arena_decay_time_read(arena_decay_t *decay) {
	return atomic_load_zd(&decay->time, ATOMIC_RELAXED);
}

static void
arena_decay_time_write(arena_decay_t *decay, ssize_t decay_time) {
	atomic_store_zd(&decay->time, decay_time, ATOMIC_RELAXED);
}

static void
arena_decay_deadline_init(arena_decay_t *decay) {
	/*
	 * Generate a new deadline that is uniformly random within the next
	 * epoch after the current one.
	 */
	nstime_copy(&decay->deadline, &decay->epoch);
	nstime_add(&decay->deadline, &decay->interval);
	if (arena_decay_time_read(decay) > 0) {
		nstime_t jitter;

		nstime_init(&jitter, prng_range_u64(&decay->jitter_state,
		    nstime_ns(&decay->interval)));
		nstime_add(&decay->deadline, &jitter);
	}
}

static bool
arena_decay_deadline_reached(const arena_decay_t *decay, const nstime_t *time) {
	return (nstime_compare(&decay->deadline, time) <= 0);
}

static size_t
arena_decay_backlog_npages_limit(const arena_decay_t *decay) {
	static const uint64_t h_steps[] = {
#define STEP(step, h, x, y) \
		h,
		SMOOTHSTEP
#undef STEP
	};
	uint64_t sum;
	size_t npages_limit_backlog;
	unsigned i;

	/*
	 * For each element of decay_backlog, multiply by the corresponding
	 * fixed-point smoothstep decay factor.  Sum the products, then divide
	 * to round down to the nearest whole number of pages.
	 */
	sum = 0;
	for (i = 0; i < SMOOTHSTEP_NSTEPS; i++) {
		sum += decay->backlog[i] * h_steps[i];
	}
	npages_limit_backlog = (size_t)(sum >> SMOOTHSTEP_BFP);

	return npages_limit_backlog;
}

static void
arena_decay_backlog_update_last(arena_decay_t *decay, extents_t *extents) {
	size_t ndirty = extents_npages_get(extents);
	size_t ndirty_delta = (ndirty > decay->nunpurged) ? ndirty -
	    decay->nunpurged : 0;
	decay->backlog[SMOOTHSTEP_NSTEPS-1] = ndirty_delta;
}

static void
arena_decay_backlog_update(arena_decay_t *decay, extents_t *extents,
    uint64_t nadvance_u64) {
	if (nadvance_u64 >= SMOOTHSTEP_NSTEPS) {
		memset(decay->backlog, 0, (SMOOTHSTEP_NSTEPS-1) *
		    sizeof(size_t));
	} else {
		size_t nadvance_z = (size_t)nadvance_u64;

		assert((uint64_t)nadvance_z == nadvance_u64);

		memmove(decay->backlog, &decay->backlog[nadvance_z],
		    (SMOOTHSTEP_NSTEPS - nadvance_z) * sizeof(size_t));
		if (nadvance_z > 1) {
			memset(&decay->backlog[SMOOTHSTEP_NSTEPS -
			    nadvance_z], 0, (nadvance_z-1) * sizeof(size_t));
		}
	}

	arena_decay_backlog_update_last(decay, extents);
}

static void
arena_decay_epoch_advance_helper(arena_decay_t *decay, extents_t *extents,
    const nstime_t *time) {
	uint64_t nadvance_u64;
	nstime_t delta;

	assert(arena_decay_deadline_reached(decay, time));

	nstime_copy(&delta, time);
	nstime_subtract(&delta, &decay->epoch);
	nadvance_u64 = nstime_divide(&delta, &decay->interval);
	assert(nadvance_u64 > 0);

	/* Add nadvance_u64 decay intervals to epoch. */
	nstime_copy(&delta, &decay->interval);
	nstime_imultiply(&delta, nadvance_u64);
	nstime_add(&decay->epoch, &delta);

	/* Set a new deadline. */
	arena_decay_deadline_init(decay);

	/* Update the backlog. */
	arena_decay_backlog_update(decay, extents, nadvance_u64);
}

static void
arena_decay_epoch_advance_purge(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, extents_t *extents) {
	size_t npages_limit = arena_decay_backlog_npages_limit(decay);

	if (extents_npages_get(extents) > npages_limit) {
		arena_decay_to_limit(tsdn, arena, decay, extents, false,
		    npages_limit);
	}
	/*
	 * There may be concurrent ndirty fluctuation between the purge above
	 * and the nunpurged update below, but this is inconsequential to decay
	 * machinery correctness.
	 */
	decay->nunpurged = extents_npages_get(extents);
}

static void
arena_decay_epoch_advance(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    extents_t *extents, const nstime_t *time) {
	arena_decay_epoch_advance_helper(decay, extents, time);
	arena_decay_epoch_advance_purge(tsdn, arena, decay, extents);
}

static void
arena_decay_reinit(arena_decay_t *decay, extents_t *extents,
    ssize_t decay_time) {
	arena_decay_time_write(decay, decay_time);
	if (decay_time > 0) {
		nstime_init2(&decay->interval, decay_time, 0);
		nstime_idivide(&decay->interval, SMOOTHSTEP_NSTEPS);
	}

	nstime_init(&decay->epoch, 0);
	nstime_update(&decay->epoch);
	decay->jitter_state = (uint64_t)(uintptr_t)decay;
	arena_decay_deadline_init(decay);
	decay->nunpurged = extents_npages_get(extents);
	memset(decay->backlog, 0, SMOOTHSTEP_NSTEPS * sizeof(size_t));
}

static bool
arena_decay_init(arena_decay_t *decay, extents_t *extents, ssize_t decay_time,
    decay_stats_t *stats) {
	if (config_debug) {
		for (size_t i = 0; i < sizeof(arena_decay_t); i++) {
			assert(((char *)decay)[i] == 0);
		}
	}
	if (malloc_mutex_init(&decay->mtx, "decay", WITNESS_RANK_DECAY)) {
		return true;
	}
	decay->purging = false;
	arena_decay_reinit(decay, extents, decay_time);
	/* Memory is zeroed, so there is no need to clear stats. */
	if (config_stats) {
		decay->stats = stats;
	}
	return false;
}

static bool
arena_decay_time_valid(ssize_t decay_time) {
	if (decay_time < -1) {
		return false;
	}
	if (decay_time == -1 || (uint64_t)decay_time <= NSTIME_SEC_MAX) {
		return true;
	}
	return false;
}

static void
arena_maybe_decay(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    extents_t *extents) {
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	/* Purge all or nothing if the option is disabled. */
	ssize_t decay_time = arena_decay_time_read(decay);
	if (decay_time <= 0) {
		if (decay_time == 0) {
			arena_decay_to_limit(tsdn, arena, decay, extents, false,
			    0);
		}
		return;
	}

	nstime_t time;
	nstime_init(&time, 0);
	nstime_update(&time);
	if (unlikely(!nstime_monotonic() && nstime_compare(&decay->epoch, &time)
	    > 0)) {
		/*
		 * Time went backwards.  Move the epoch back in time and
		 * generate a new deadline, with the expectation that time
		 * typically flows forward for long enough periods of time that
		 * epochs complete.  Unfortunately, this strategy is susceptible
		 * to clock jitter triggering premature epoch advances, but
		 * clock jitter estimation and compensation isn't feasible here
		 * because calls into this code are event-driven.
		 */
		nstime_copy(&decay->epoch, &time);
		arena_decay_deadline_init(decay);
	} else {
		/* Verify that time does not go backwards. */
		assert(nstime_compare(&decay->epoch, &time) <= 0);
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances.
	 */
	if (arena_decay_deadline_reached(decay, &time)) {
		arena_decay_epoch_advance(tsdn, arena, decay, extents, &time);
	}
}

static ssize_t
arena_decay_time_get(arena_decay_t *decay) {
	return arena_decay_time_read(decay);
}

ssize_t
arena_dirty_decay_time_get(arena_t *arena) {
	return arena_decay_time_get(&arena->decay_dirty);
}

ssize_t
arena_muzzy_decay_time_get(arena_t *arena) {
	return arena_decay_time_get(&arena->decay_muzzy);
}

static bool
arena_decay_time_set(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    extents_t *extents, ssize_t decay_time) {
	if (!arena_decay_time_valid(decay_time)) {
		return true;
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_time changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	arena_decay_reinit(decay, extents, decay_time);
	arena_maybe_decay(tsdn, arena, decay, extents);
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return false;
}

bool
arena_dirty_decay_time_set(tsdn_t *tsdn, arena_t *arena, ssize_t decay_time) {
	return arena_decay_time_set(tsdn, arena, &arena->decay_dirty,
	    &arena->extents_dirty, decay_time);
}

bool
arena_muzzy_decay_time_set(tsdn_t *tsdn, arena_t *arena, ssize_t decay_time) {
	return arena_decay_time_set(tsdn, arena, &arena->decay_muzzy,
	    &arena->extents_muzzy, decay_time);
}

static size_t
arena_stash_decayed(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extents_t *extents, size_t npages_limit,
    extent_list_t *decay_extents) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	/* Stash extents according to npages_limit. */
	size_t nstashed = 0;
	extent_t *extent;
	while ((extent = extents_evict(tsdn, arena, r_extent_hooks, extents,
	    npages_limit)) != NULL) {
		extent_list_append(decay_extents, extent);
		nstashed += extent_size_get(extent) >> LG_PAGE;
	}
	return nstashed;
}

static size_t
arena_decay_stashed(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, arena_decay_t *decay, extents_t *extents,
    bool all, extent_list_t *decay_extents) {
	UNUSED size_t nmadvise, nunmapped;
	size_t npurged;

	if (config_stats) {
		nmadvise = 0;
		nunmapped = 0;
	}
	npurged = 0;

	ssize_t muzzy_decay_time = arena_muzzy_decay_time_get(arena);
	for (extent_t *extent = extent_list_first(decay_extents); extent !=
	    NULL; extent = extent_list_first(decay_extents)) {
		if (config_stats) {
			nmadvise++;
		}
		size_t npages = extent_size_get(extent) >> LG_PAGE;
		npurged += npages;
		extent_list_remove(decay_extents, extent);
		switch (extents_state_get(extents)) {
		case extent_state_active:
			not_reached();
		case extent_state_dirty:
			if (!all && muzzy_decay_time != 0 &&
			    !extent_purge_lazy_wrapper(tsdn, arena,
			    r_extent_hooks, extent, 0,
			    extent_size_get(extent))) {
				extents_dalloc(tsdn, arena, r_extent_hooks,
				    &arena->extents_muzzy, extent);
				break;
			}
			/* Fall through. */
		case extent_state_muzzy:
			extent_dalloc_wrapper(tsdn, arena, r_extent_hooks,
			    extent);
			if (config_stats) {
				nunmapped += npages;
			}
			break;
		case extent_state_retained:
		default:
			not_reached();
		}
	}

	if (config_stats) {
		arena_stats_lock(tsdn, &arena->stats);
		arena_stats_add_u64(tsdn, &arena->stats, &decay->stats->npurge,
		    1);
		arena_stats_add_u64(tsdn, &arena->stats,
		    &decay->stats->nmadvise, nmadvise);
		arena_stats_add_u64(tsdn, &arena->stats, &decay->stats->purged,
		    npurged);
		arena_stats_sub_zu(tsdn, &arena->stats, &arena->stats.mapped,
		    nunmapped);
		arena_stats_unlock(tsdn, &arena->stats);
	}

	return npurged;
}

/*
 * npages_limit: Decay as many dirty extents as possible without violating the
 * invariant: (extents_npages_get(extents) >= npages_limit)
 */
static void
arena_decay_to_limit(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    extents_t *extents, bool all, size_t npages_limit) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 1);
	malloc_mutex_assert_owner(tsdn, &decay->mtx);

	if (decay->purging) {
		return;
	}
	decay->purging = true;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	extent_hooks_t *extent_hooks = extent_hooks_get(arena);

	extent_list_t decay_extents;
	extent_list_init(&decay_extents);

	size_t npurge = arena_stash_decayed(tsdn, arena, &extent_hooks, extents,
	    npages_limit, &decay_extents);
	if (npurge != 0) {
		UNUSED size_t npurged = arena_decay_stashed(tsdn, arena,
		    &extent_hooks, decay, extents, all, &decay_extents);
		assert(npurged == npurge);
	}

	malloc_mutex_lock(tsdn, &decay->mtx);
	decay->purging = false;
}

static void
arena_decay_impl(tsdn_t *tsdn, arena_t *arena, arena_decay_t *decay,
    extents_t *extents, bool all) {
	malloc_mutex_lock(tsdn, &decay->mtx);
	if (all) {
		arena_decay_to_limit(tsdn, arena, decay, extents, all, 0);
	} else {
		arena_maybe_decay(tsdn, arena, decay, extents);
	}
	malloc_mutex_unlock(tsdn, &decay->mtx);
}

static void
arena_decay_dirty(tsdn_t *tsdn, arena_t *arena, bool all) {
	arena_decay_impl(tsdn, arena, &arena->decay_dirty,
	    &arena->extents_dirty, all);
}

static void
arena_decay_muzzy(tsdn_t *tsdn, arena_t *arena, bool all) {
	arena_decay_impl(tsdn, arena, &arena->decay_muzzy,
	    &arena->extents_muzzy, all);
}

void
arena_decay(tsdn_t *tsdn, arena_t *arena, bool all) {
	arena_decay_dirty(tsdn, arena, all);
	arena_decay_muzzy(tsdn, arena, all);
}

static void
arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *slab) {
	arena_nactive_sub(arena, extent_size_get(slab) >> LG_PAGE);

	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;
	arena_extents_dirty_dalloc(tsdn, arena, &extent_hooks, slab);
}

static void
arena_bin_slabs_nonfull_insert(arena_bin_t *bin, extent_t *slab) {
	assert(extent_slab_data_get(slab)->nfree > 0);
	extent_heap_insert(&bin->slabs_nonfull, slab);
}

static void
arena_bin_slabs_nonfull_remove(arena_bin_t *bin, extent_t *slab) {
	extent_heap_remove(&bin->slabs_nonfull, slab);
}

static extent_t *
arena_bin_slabs_nonfull_tryget(arena_bin_t *bin) {
	extent_t *slab = extent_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.reslabs++;
	}
	return slab;
}

static void
arena_bin_slabs_full_insert(arena_bin_t *bin, extent_t *slab) {
	assert(extent_slab_data_get(slab)->nfree == 0);
	extent_list_append(&bin->slabs_full, slab);
}

static void
arena_bin_slabs_full_remove(arena_bin_t *bin, extent_t *slab) {
	extent_list_remove(&bin->slabs_full, slab);
}

void
arena_reset(tsd_t *tsd, arena_t *arena) {
	/*
	 * Locking in this function is unintuitive.  The caller guarantees that
	 * no concurrent operations are happening in this arena, but there are
	 * still reasons that some locking is necessary:
	 *
	 * - Some of the functions in the transitive closure of calls assume
	 *   appropriate locks are held, and in some cases these locks are
	 *   temporarily dropped to avoid lock order reversal or deadlock due to
	 *   reentry.
	 * - mallctl("epoch", ...) may concurrently refresh stats.  While
	 *   strictly speaking this is a "concurrent operation", disallowing
	 *   stats refreshes would impose an inconvenient burden.
	 */

	/* Large allocations. */
	malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);

	for (extent_t *extent = extent_list_first(&arena->large); extent !=
	    NULL; extent = extent_list_first(&arena->large)) {
		void *ptr = extent_base_get(extent);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);
		if (config_stats || (config_prof && opt_prof)) {
			usize = isalloc(tsd_tsdn(tsd), ptr);
		}
		/* Remove large allocation from prof sample set. */
		if (config_prof && opt_prof) {
			prof_free(tsd, extent, ptr, usize);
		}
		large_dalloc(tsd_tsdn(tsd), extent);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);

	/* Bins. */
	for (unsigned i = 0; i < NBINS; i++) {
		extent_t *slab;
		arena_bin_t *bin = &arena->bins[i];
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		if (bin->slabcur != NULL) {
			slab = bin->slabcur;
			bin->slabcur = NULL;
			malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
			arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
			malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		}
		while ((slab = extent_heap_remove_first(&bin->slabs_nonfull)) !=
		    NULL) {
			malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
			arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
			malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		}
		for (slab = extent_list_first(&bin->slabs_full); slab != NULL;
		    slab = extent_list_first(&bin->slabs_full)) {
			arena_bin_slabs_full_remove(bin, slab);
			malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
			arena_slab_dalloc(tsd_tsdn(tsd), arena, slab);
			malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		}
		if (config_stats) {
			bin->stats.curregs = 0;
			bin->stats.curslabs = 0;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
	}

	atomic_write_zu(&arena->nactive, 0);
}

static void
arena_destroy_retained(tsdn_t *tsdn, arena_t *arena) {
	/*
	 * Iterate over the retained extents and blindly attempt to deallocate
	 * them.  This gives the extent allocator underlying the extent hooks an
	 * opportunity to unmap all retained memory without having to keep its
	 * own metadata structures, but if deallocation fails, that is the
	 * application's decision/problem.  In practice, retained extents are
	 * leaked here if !config_munmap unless the application provided custom
	 * extent hooks, so best practice is to either enable munmap (and avoid
	 * dss for arenas to be destroyed), or provide custom extent hooks that
	 * either unmap retained extents or track them for later use.
	 */
	extent_hooks_t *extent_hooks = extent_hooks_get(arena);
	extent_t *extent;
	while ((extent = extents_evict(tsdn, arena, &extent_hooks,
	    &arena->extents_retained, 0)) != NULL) {
		extent_dalloc_wrapper_try(tsdn, arena, &extent_hooks, extent);
	}
}

void
arena_destroy(tsd_t *tsd, arena_t *arena) {
	assert(base_ind_get(arena->base) >= narenas_auto);
	assert(arena_nthreads_get(arena, false) == 0);
	assert(arena_nthreads_get(arena, true) == 0);

	/*
	 * No allocations have occurred since arena_reset() was called.
	 * Furthermore, the caller (arena_i_destroy_ctl()) purged all cached
	 * extents, so only retained extents may remain.
	 */
	assert(extents_npages_get(&arena->extents_dirty) == 0);

	/* Attempt to deallocate retained memory. */
	arena_destroy_retained(tsd_tsdn(tsd), arena);

	/*
	 * Remove the arena pointer from the arenas array.  We rely on the fact
	 * that there is no way for the application to get a dirty read from the
	 * arenas array unless there is an inherent race in the application
	 * involving access of an arena being concurrently destroyed.  The
	 * application must synchronize knowledge of the arena's validity, so as
	 * long as we use an atomic write to update the arenas array, the
	 * application will get a clean read any time after it synchronizes
	 * knowledge that the arena is no longer valid.
	 */
	arena_set(base_ind_get(arena->base), NULL);

	/*
	 * Destroy the base allocator, which manages all metadata ever mapped by
	 * this arena.
	 */
	base_delete(arena->base);
}

static extent_t *
arena_slab_alloc_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, const arena_bin_info_t *bin_info,
    szind_t szind) {
	extent_t *slab;
	bool zero, commit;

	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	zero = false;
	commit = true;
	slab = extent_alloc_wrapper(tsdn, arena, r_extent_hooks, NULL,
	    bin_info->slab_size, 0, PAGE, true, szind, &zero, &commit);

	if (config_stats && slab != NULL) {
		arena_stats_mapped_add(tsdn, &arena->stats,
		    bin_info->slab_size);
	}

	return slab;
}

static extent_t *
arena_slab_alloc(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    const arena_bin_info_t *bin_info) {
	witness_assert_depth_to_rank(tsdn, WITNESS_RANK_CORE, 0);

	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;
	szind_t szind = size2index(bin_info->reg_size);
	bool zero = false;
	bool commit = true;
	extent_t *slab = extents_alloc(tsdn, arena, &extent_hooks,
	    &arena->extents_dirty, NULL, bin_info->slab_size, 0, PAGE, true,
	    binind, &zero, &commit);
	if (slab == NULL) {
		slab = extents_alloc(tsdn, arena, &extent_hooks,
		    &arena->extents_muzzy, NULL, bin_info->slab_size, 0, PAGE,
		    true, binind, &zero, &commit);
	}
	if (slab == NULL) {
		slab = arena_slab_alloc_hard(tsdn, arena, &extent_hooks,
		    bin_info, szind);
		if (slab == NULL) {
			return NULL;
		}
	}
	assert(extent_slab_get(slab));

	/* Initialize slab internals. */
	arena_slab_data_t *slab_data = extent_slab_data_get(slab);
	slab_data->nfree = bin_info->nregs;
	bitmap_init(slab_data->bitmap, &bin_info->bitmap_info);

	arena_nactive_add(arena, extent_size_get(slab) >> LG_PAGE);

	return slab;
}

static extent_t *
arena_bin_nonfull_slab_get(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin,
    szind_t binind) {
	extent_t *slab;
	const arena_bin_info_t *bin_info;

	/* Look for a usable slab. */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL) {
		return slab;
	}
	/* No existing slabs have any space available. */

	bin_info = &arena_bin_info[binind];

	/* Allocate a new slab. */
	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	slab = arena_slab_alloc(tsdn, arena, binind, bin_info);
	/********************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (slab != NULL) {
		if (config_stats) {
			bin->stats.nslabs++;
			bin->stats.curslabs++;
		}
		return slab;
	}

	/*
	 * arena_slab_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL) {
		return slab;
	}

	return NULL;
}

/* Re-fill bin->slabcur, then call arena_slab_reg_alloc(). */
static void *
arena_bin_malloc_hard(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin,
    szind_t binind) {
	const arena_bin_info_t *bin_info;
	extent_t *slab;

	bin_info = &arena_bin_info[binind];
	if (bin->slabcur != NULL) {
		arena_bin_slabs_full_insert(bin, bin->slabcur);
		bin->slabcur = NULL;
	}
	slab = arena_bin_nonfull_slab_get(tsdn, arena, bin, binind);
	if (bin->slabcur != NULL) {
		/*
		 * Another thread updated slabcur while this one ran without the
		 * bin lock in arena_bin_nonfull_slab_get().
		 */
		if (extent_slab_data_get(bin->slabcur)->nfree > 0) {
			void *ret = arena_slab_reg_alloc(tsdn, bin->slabcur,
			    bin_info);
			if (slab != NULL) {
				/*
				 * arena_slab_alloc() may have allocated slab,
				 * or it may have been pulled from
				 * slabs_nonfull.  Therefore it is unsafe to
				 * make any assumptions about how slab has
				 * previously been used, and
				 * arena_bin_lower_slab() must be called, as if
				 * a region were just deallocated from the slab.
				 */
				if (extent_slab_data_get(slab)->nfree ==
				    bin_info->nregs) {
					arena_dalloc_bin_slab(tsdn, arena, slab,
					    bin);
				} else {
					arena_bin_lower_slab(tsdn, arena, slab,
					    bin);
				}
			}
			return ret;
		}

		arena_bin_slabs_full_insert(bin, bin->slabcur);
		bin->slabcur = NULL;
	}

	if (slab == NULL) {
		return NULL;
	}
	bin->slabcur = slab;

	assert(extent_slab_data_get(bin->slabcur)->nfree > 0);

	return arena_slab_reg_alloc(tsdn, slab, bin_info);
}

void
arena_tcache_fill_small(tsdn_t *tsdn, arena_t *arena, tcache_bin_t *tbin,
    szind_t binind, uint64_t prof_accumbytes) {
	unsigned i, nfill;
	arena_bin_t *bin;

	assert(tbin->ncached == 0);

	if (config_prof && arena_prof_accum(tsdn, arena, prof_accumbytes)) {
		prof_idump(tsdn);
	}
	bin = &arena->bins[binind];
	malloc_mutex_lock(tsdn, &bin->lock);
	for (i = 0, nfill = (tcache_bin_info[binind].ncached_max >>
	    tbin->lg_fill_div); i < nfill; i++) {
		extent_t *slab;
		void *ptr;
		if ((slab = bin->slabcur) != NULL &&
		    extent_slab_data_get(slab)->nfree > 0) {
			ptr = arena_slab_reg_alloc(tsdn, slab,
			    &arena_bin_info[binind]);
		} else {
			ptr = arena_bin_malloc_hard(tsdn, arena, bin, binind);
		}
		if (ptr == NULL) {
			/*
			 * OOM.  tbin->avail isn't yet filled down to its first
			 * element, so the successful allocations (if any) must
			 * be moved just before tbin->avail before bailing out.
			 */
			if (i > 0) {
				memmove(tbin->avail - i, tbin->avail - nfill,
				    i * sizeof(void *));
			}
			break;
		}
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ptr, &arena_bin_info[binind],
			    true);
		}
		/* Insert such that low regions get used first. */
		*(tbin->avail - nfill + i) = ptr;
	}
	if (config_stats) {
		bin->stats.nmalloc += i;
		bin->stats.nrequests += tbin->tstats.nrequests;
		bin->stats.curregs += i;
		bin->stats.nfills++;
		tbin->tstats.nrequests = 0;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);
	tbin->ncached = i;
	arena_decay_tick(tsdn, arena);
}

void
arena_alloc_junk_small(void *ptr, const arena_bin_info_t *bin_info, bool zero) {
	if (!zero) {
		memset(ptr, JEMALLOC_ALLOC_JUNK, bin_info->reg_size);
	}
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define arena_dalloc_junk_small JEMALLOC_N(n_arena_dalloc_junk_small)
#endif
void
arena_dalloc_junk_small(void *ptr, const arena_bin_info_t *bin_info) {
	memset(ptr, JEMALLOC_FREE_JUNK, bin_info->reg_size);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small)
arena_dalloc_junk_small_t *arena_dalloc_junk_small =
    JEMALLOC_N(n_arena_dalloc_junk_small);
#endif

static void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero) {
	void *ret;
	arena_bin_t *bin;
	size_t usize;
	extent_t *slab;

	assert(binind < NBINS);
	bin = &arena->bins[binind];
	usize = index2size(binind);

	malloc_mutex_lock(tsdn, &bin->lock);
	if ((slab = bin->slabcur) != NULL && extent_slab_data_get(slab)->nfree >
	    0) {
		ret = arena_slab_reg_alloc(tsdn, slab, &arena_bin_info[binind]);
	} else {
		ret = arena_bin_malloc_hard(tsdn, arena, bin, binind);
	}

	if (ret == NULL) {
		malloc_mutex_unlock(tsdn, &bin->lock);
		return NULL;
	}

	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);
	if (config_prof && arena_prof_accum(tsdn, arena, usize)) {
		prof_idump(tsdn);
	}

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc)) {
				arena_alloc_junk_small(ret,
				    &arena_bin_info[binind], false);
			} else if (unlikely(opt_zero)) {
				memset(ret, 0, usize);
			}
		}
	} else {
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ret, &arena_bin_info[binind],
			    true);
		}
		memset(ret, 0, usize);
	}

	arena_decay_tick(tsdn, arena);
	return ret;
}

void *
arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero) {
	assert(!tsdn_null(tsdn) || arena != NULL);

	if (likely(!tsdn_null(tsdn))) {
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	}
	if (unlikely(arena == NULL)) {
		return NULL;
	}

	if (likely(size <= SMALL_MAXCLASS)) {
		return arena_malloc_small(tsdn, arena, ind, zero);
	}
	return large_malloc(tsdn, arena, index2size(ind), zero);
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache) {
	void *ret;

	if (usize <= SMALL_MAXCLASS && (alignment < PAGE || (alignment == PAGE
	    && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special slab placement. */
		ret = arena_malloc(tsdn, arena, usize, size2index(usize), zero,
		    tcache, true);
	} else {
		if (likely(alignment <= CACHELINE)) {
			ret = large_malloc(tsdn, arena, usize, zero);
		} else {
			ret = large_palloc(tsdn, arena, usize, alignment, zero);
		}
	}
	return ret;
}

void
arena_prof_promote(tsdn_t *tsdn, extent_t *extent, const void *ptr,
    size_t usize) {
	arena_t *arena = extent_arena_get(extent);

	cassert(config_prof);
	assert(ptr != NULL);
	assert(isalloc(tsdn, ptr) == LARGE_MINCLASS);
	assert(usize <= SMALL_MAXCLASS);

	szind_t szind = size2index(usize);
	extent_szind_set(extent, szind);
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    szind, false);

	prof_accum_cancel(tsdn, &arena->prof_accum, usize);

	assert(isalloc(tsdn, ptr) == usize);
}

static size_t
arena_prof_demote(tsdn_t *tsdn, extent_t *extent, const void *ptr) {
	cassert(config_prof);
	assert(ptr != NULL);

	extent_szind_set(extent, NBINS);
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
	rtree_szind_slab_update(tsdn, &extents_rtree, rtree_ctx, (uintptr_t)ptr,
	    NBINS, false);

	assert(isalloc(tsdn, ptr) == LARGE_MINCLASS);

	return LARGE_MINCLASS;
}

void
arena_dalloc_promoted(tsdn_t *tsdn, extent_t *extent, void *ptr,
    tcache_t *tcache, bool slow_path) {
	size_t usize;

	cassert(config_prof);
	assert(opt_prof);

	usize = arena_prof_demote(tsdn, extent, ptr);
	if (usize <= tcache_maxclass) {
		tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr,
		    size2index(usize), slow_path);
	} else {
		large_dalloc(tsdn, extent);
	}
}

static void
arena_dissociate_bin_slab(extent_t *slab, arena_bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t binind = extent_szind_get(slab);
		const arena_bin_info_t *bin_info = &arena_bin_info[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			arena_bin_slabs_full_remove(bin, slab);
		} else {
			arena_bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

static void
arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin) {
	assert(slab != bin->slabcur);

	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	arena_slab_dalloc(tsdn, arena, slab);
	/****************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

static void
arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin) {
	assert(extent_slab_data_get(slab)->nfree > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the
	 * oldest/lowest non-full slab.  It is okay to NULL slabcur out rather
	 * than proactively keeping it pointing at the oldest/lowest non-full
	 * slab.
	 */
	if (bin->slabcur != NULL && extent_snad_comp(bin->slabcur, slab) > 0) {
		/* Switch slabcur. */
		if (extent_slab_data_get(bin->slabcur)->nfree > 0) {
			arena_bin_slabs_nonfull_insert(bin, bin->slabcur);
		} else {
			arena_bin_slabs_full_insert(bin, bin->slabcur);
		}
		bin->slabcur = slab;
		if (config_stats) {
			bin->stats.reslabs++;
		}
	} else {
		arena_bin_slabs_nonfull_insert(bin, slab);
	}
}

static void
arena_dalloc_bin_locked_impl(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    void *ptr, bool junked) {
	arena_slab_data_t *slab_data = extent_slab_data_get(slab);
	szind_t binind = extent_szind_get(slab);
	arena_bin_t *bin = &arena->bins[binind];
	const arena_bin_info_t *bin_info = &arena_bin_info[binind];

	if (!junked && config_fill && unlikely(opt_junk_free)) {
		arena_dalloc_junk_small(ptr, bin_info);
	}

	arena_slab_reg_dalloc(tsdn, slab, slab_data, ptr);
	if (slab_data->nfree == bin_info->nregs) {
		arena_dissociate_bin_slab(slab, bin);
		arena_dalloc_bin_slab(tsdn, arena, slab, bin);
	} else if (slab_data->nfree == 1 && slab != bin->slabcur) {
		arena_bin_slabs_full_remove(bin, slab);
		arena_bin_lower_slab(tsdn, arena, slab, bin);
	}

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}
}

void
arena_dalloc_bin_junked_locked(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    void *ptr) {
	arena_dalloc_bin_locked_impl(tsdn, arena, extent, ptr, true);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr) {
	szind_t binind = extent_szind_get(extent);
	arena_bin_t *bin = &arena->bins[binind];

	malloc_mutex_lock(tsdn, &bin->lock);
	arena_dalloc_bin_locked_impl(tsdn, arena, extent, ptr, false);
	malloc_mutex_unlock(tsdn, &bin->lock);
}

void
arena_dalloc_small(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr) {
	arena_dalloc_bin(tsdn, arena, extent, ptr);
	arena_decay_tick(tsdn, arena);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, extent_t *extent, void *ptr, size_t oldsize,
    size_t size, size_t extra, bool zero) {
	size_t usize_min, usize_max;

	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= LARGE_MAXCLASS);

	if (unlikely(size > LARGE_MAXCLASS)) {
		return true;
	}

	usize_min = s2u(size);
	usize_max = s2u(size + extra);
	if (likely(oldsize <= SMALL_MAXCLASS && usize_min <= SMALL_MAXCLASS)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		assert(arena_bin_info[size2index(oldsize)].reg_size ==
		    oldsize);
		if ((usize_max > SMALL_MAXCLASS || size2index(usize_max) !=
		    size2index(oldsize)) && (size > oldsize || usize_max <
		    oldsize)) {
			return true;
		}

		arena_decay_tick(tsdn, extent_arena_get(extent));
		return false;
	} else if (oldsize >= LARGE_MINCLASS && usize_max >= LARGE_MINCLASS) {
		return large_ralloc_no_move(tsdn, extent, usize_min, usize_max,
		    zero);
	}

	return true;
}

static void *
arena_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache) {
	if (alignment == 0) {
		return arena_malloc(tsdn, arena, usize, size2index(usize),
		    zero, tcache, true);
	}
	usize = sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > LARGE_MAXCLASS)) {
		return NULL;
	}
	return ipalloct(tsdn, usize, alignment, zero, tcache, arena);
}

void *
arena_ralloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr,
    size_t oldsize, size_t size, size_t alignment, bool zero,
    tcache_t *tcache) {
	void *ret;
	size_t usize, copysize;

	usize = s2u(size);
	if (unlikely(usize == 0 || size > LARGE_MAXCLASS)) {
		return NULL;
	}

	if (likely(usize <= SMALL_MAXCLASS)) {
		/* Try to avoid moving the allocation. */
		if (!arena_ralloc_no_move(tsdn, extent, ptr, oldsize, usize, 0,
		    zero)) {
			return ptr;
		}
	}

	if (oldsize >= LARGE_MINCLASS && usize >= LARGE_MINCLASS) {
		return large_ralloc(tsdn, arena, extent, usize, alignment,
		    zero, tcache);
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	ret = arena_ralloc_move_helper(tsdn, arena, usize, alignment, zero,
	    tcache);
	if (ret == NULL) {
		return NULL;
	}

	/*
	 * Junk/zero-filling were already done by
	 * ipalloc()/arena_malloc().
	 */

	copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, ptr, oldsize, tcache, true);
	return ret;
}

dss_prec_t
arena_dss_prec_get(arena_t *arena) {
	return (dss_prec_t)atomic_read_u((unsigned *)&arena->dss_prec);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec) {
	if (!have_dss) {
		return (dss_prec != dss_prec_disabled);
	}
	atomic_write_u((unsigned *)&arena->dss_prec, dss_prec);
	return false;
}

ssize_t
arena_dirty_decay_time_default_get(void) {
	return (ssize_t)atomic_read_zu((size_t *)&dirty_decay_time_default);
}

bool
arena_dirty_decay_time_default_set(ssize_t decay_time) {
	if (!arena_decay_time_valid(decay_time)) {
		return true;
	}
	atomic_write_zu((size_t *)&dirty_decay_time_default,
	    (size_t)decay_time);
	return false;
}

ssize_t
arena_muzzy_decay_time_default_get(void) {
	return (ssize_t)atomic_read_zu((size_t *)&muzzy_decay_time_default);
}

bool
arena_muzzy_decay_time_default_set(ssize_t decay_time) {
	if (!arena_decay_time_valid(decay_time)) {
		return true;
	}
	atomic_write_zu((size_t *)&muzzy_decay_time_default,
	    (size_t)decay_time);
	return false;
}

unsigned
arena_nthreads_get(arena_t *arena, bool internal) {
	return atomic_read_u(&arena->nthreads[internal]);
}

void
arena_nthreads_inc(arena_t *arena, bool internal) {
	atomic_add_u(&arena->nthreads[internal], 1);
}

void
arena_nthreads_dec(arena_t *arena, bool internal) {
	atomic_sub_u(&arena->nthreads[internal], 1);
}

size_t
arena_extent_sn_next(arena_t *arena) {
	return atomic_add_zu(&arena->extent_sn_next, 1) - 1;
}

arena_t *
arena_new(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;
	base_t *base;
	unsigned i;

	if (ind == 0) {
		base = b0get();
	} else {
		base = base_new(tsdn, ind, extent_hooks);
		if (base == NULL) {
			return NULL;
		}
	}

	arena = (arena_t *)base_alloc(tsdn, base, sizeof(arena_t), CACHELINE);
	if (arena == NULL) {
		goto label_error;
	}

	arena->nthreads[0] = arena->nthreads[1] = 0;
	arena->last_thd = NULL;

	if (config_stats) {
		if (arena_stats_init(tsdn, &arena->stats)) {
			goto label_error;
		}
	}

	if (config_stats && config_tcache) {
		ql_new(&arena->tcache_ql);
		if (malloc_mutex_init(&arena->tcache_ql_mtx, "tcache_ql",
		    WITNESS_RANK_TCACHE_QL)) {
			goto label_error;
		}
	}

	if (config_prof) {
		if (prof_accum_init(tsdn, &arena->prof_accum)) {
			goto label_error;
		}
	}

	if (config_cache_oblivious) {
		/*
		 * A nondeterministic seed based on the address of arena reduces
		 * the likelihood of lockstep non-uniform cache index
		 * utilization among identical concurrent processes, but at the
		 * cost of test repeatability.  For debug builds, instead use a
		 * deterministic seed.
		 */
		arena->offset_state = config_debug ? ind :
		    (size_t)(uintptr_t)arena;
	}

	arena->extent_sn_next = 0;

	arena->dss_prec = extent_dss_prec_get();

	atomic_write_zu(&arena->nactive, 0);

	extent_list_init(&arena->large);
	if (malloc_mutex_init(&arena->large_mtx, "arena_large",
	    WITNESS_RANK_ARENA_LARGE)) {
		goto label_error;
	}

	/*
	 * Delay coalescing for dirty extents despite the disruptive effect on
	 * memory layout for best-fit extent allocation, since cached extents
	 * are likely to be reused soon after deallocation, and the cost of
	 * merging/splitting extents is non-trivial.
	 */
	if (extents_init(tsdn, &arena->extents_dirty, extent_state_dirty,
	    true)) {
		goto label_error;
	}
	/*
	 * Coalesce muzzy extents immediately, because operations on them are in
	 * the critical path much less often than for dirty extents.
	 */
	if (extents_init(tsdn, &arena->extents_muzzy, extent_state_muzzy,
	    false)) {
		goto label_error;
	}
	/*
	 * Coalesce retained extents immediately, in part because they will
	 * never be evicted (and therefore there's no opportunity for delayed
	 * coalescing), but also because operations on retained extents are not
	 * in the critical path.
	 */
	if (extents_init(tsdn, &arena->extents_retained, extent_state_retained,
	    false)) {
		goto label_error;
	}

	if (arena_decay_init(&arena->decay_dirty, &arena->extents_dirty,
	    arena_dirty_decay_time_default_get(), &arena->stats.decay_dirty)) {
		goto label_error;
	}
	if (arena_decay_init(&arena->decay_muzzy, &arena->extents_muzzy,
	    arena_muzzy_decay_time_default_get(), &arena->stats.decay_muzzy)) {
		goto label_error;
	}

	if (!config_munmap) {
		arena->extent_grow_next = psz2ind(HUGEPAGE);
	}

	extent_list_init(&arena->extent_freelist);
	if (malloc_mutex_init(&arena->extent_freelist_mtx, "extent_freelist",
	    WITNESS_RANK_EXTENT_FREELIST)) {
		goto label_error;
	}

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock, "arena_bin",
		    WITNESS_RANK_ARENA_BIN)) {
			goto label_error;
		}
		bin->slabcur = NULL;
		extent_heap_new(&bin->slabs_nonfull);
		extent_list_init(&bin->slabs_full);
		if (config_stats) {
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
		}
	}

	arena->base = base;

	return arena;
label_error:
	if (ind != 0) {
		base_delete(base);
	}
	return NULL;
}

void
arena_boot(void) {
	arena_dirty_decay_time_default_set(opt_dirty_decay_time);
	arena_muzzy_decay_time_default_set(opt_muzzy_decay_time);
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_prefork(tsdn, &arena->decay_muzzy.mtx);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena) {
	if (config_stats && config_tcache) {
		malloc_mutex_prefork(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_prefork2(tsdn_t *tsdn, arena_t *arena) {
	extents_prefork(tsdn, &arena->extents_dirty);
	extents_prefork(tsdn, &arena->extents_muzzy);
	extents_prefork(tsdn, &arena->extents_retained);
}

void
arena_prefork3(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->extent_freelist_mtx);
}

void
arena_prefork4(tsdn_t *tsdn, arena_t *arena) {
	base_prefork(tsdn, arena->base);
}

void
arena_prefork5(tsdn_t *tsdn, arena_t *arena) {
	malloc_mutex_prefork(tsdn, &arena->large_mtx);
}

void
arena_prefork6(tsdn_t *tsdn, arena_t *arena) {
	for (unsigned i = 0; i < NBINS; i++) {
		malloc_mutex_prefork(tsdn, &arena->bins[i].lock);
	}
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	for (i = 0; i < NBINS; i++) {
		malloc_mutex_postfork_parent(tsdn, &arena->bins[i].lock);
	}
	malloc_mutex_postfork_parent(tsdn, &arena->large_mtx);
	base_postfork_parent(tsdn, arena->base);
	malloc_mutex_postfork_parent(tsdn, &arena->extent_freelist_mtx);
	extents_postfork_parent(tsdn, &arena->extents_dirty);
	extents_postfork_parent(tsdn, &arena->extents_muzzy);
	extents_postfork_parent(tsdn, &arena->extents_retained);
	malloc_mutex_postfork_parent(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->decay_muzzy.mtx);
	if (config_stats && config_tcache) {
		malloc_mutex_postfork_parent(tsdn, &arena->tcache_ql_mtx);
	}
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena) {
	unsigned i;

	for (i = 0; i < NBINS; i++) {
		malloc_mutex_postfork_child(tsdn, &arena->bins[i].lock);
	}
	malloc_mutex_postfork_child(tsdn, &arena->large_mtx);
	base_postfork_child(tsdn, arena->base);
	malloc_mutex_postfork_child(tsdn, &arena->extent_freelist_mtx);
	extents_postfork_child(tsdn, &arena->extents_dirty);
	extents_postfork_child(tsdn, &arena->extents_muzzy);
	extents_postfork_child(tsdn, &arena->extents_retained);
	malloc_mutex_postfork_child(tsdn, &arena->decay_dirty.mtx);
	malloc_mutex_postfork_child(tsdn, &arena->decay_muzzy.mtx);
	if (config_stats && config_tcache) {
		malloc_mutex_postfork_child(tsdn, &arena->tcache_ql_mtx);
	}
}
