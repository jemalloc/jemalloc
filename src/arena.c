#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

ssize_t		opt_decay_time = DECAY_TIME_DEFAULT;
static ssize_t	decay_time_default;

const arena_bin_info_t	arena_bin_info[NBINS] = {
#define	BIN_INFO_bin_yes(reg_size, slab_size, nregs)			\
	{reg_size, slab_size, nregs, BITMAP_INFO_INITIALIZER(nregs)},
#define	BIN_INFO_bin_no(reg_size, slab_size, nregs)
#define	SC(index, lg_grp, lg_delta, ndelta, psz, bin, pgs,		\
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

static void	arena_purge_to_limit(tsdn_t *tsdn, arena_t *arena,
    size_t ndirty_limit);
static void	arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena,
    extent_t *slab, arena_bin_t *bin);
static void	arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena,
    extent_t *slab, arena_bin_t *bin);

/******************************************************************************/

static size_t
arena_extent_dirty_npages(const extent_t *extent)
{

	return (extent_size_get(extent) >> LG_PAGE);
}

static extent_t *
arena_extent_cache_alloc_locked(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool slab)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	return (extent_alloc_cache(tsdn, arena, r_extent_hooks, new_addr, usize,
	    pad, alignment, zero, slab));
}

extent_t *
arena_extent_cache_alloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero)
{
	extent_t *extent;

	malloc_mutex_lock(tsdn, &arena->lock);
	extent = arena_extent_cache_alloc_locked(tsdn, arena, r_extent_hooks,
	    new_addr, size, 0, alignment, zero, false);
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (extent);
}

static void
arena_extent_cache_dalloc_locked(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	extent_dalloc_cache(tsdn, arena, r_extent_hooks, extent);
	arena_maybe_purge(tsdn, arena);
}

void
arena_extent_cache_dalloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_extent_cache_dalloc_locked(tsdn, arena, r_extent_hooks, extent);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_extent_cache_maybe_insert(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    bool cache)
{

	malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);

	if (cache) {
		extent_ring_insert(&arena->extents_dirty, extent);
		arena->ndirty += arena_extent_dirty_npages(extent);
	}
}

void
arena_extent_cache_maybe_remove(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    bool dirty)
{

	malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);

	if (dirty) {
		extent_ring_remove(extent);
		assert(arena->ndirty >= arena_extent_dirty_npages(extent));
		arena->ndirty -= arena_extent_dirty_npages(extent);
	}
}

JEMALLOC_INLINE_C void *
arena_slab_reg_alloc(tsdn_t *tsdn, extent_t *slab,
    const arena_bin_info_t *bin_info)
{
	void *ret;
	arena_slab_data_t *slab_data = extent_slab_data_get(slab);
	size_t regind;

	assert(slab_data->nfree > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = (unsigned)bitmap_sfu(slab_data->bitmap,
	    &bin_info->bitmap_info);
	ret = (void *)((uintptr_t)extent_addr_get(slab) +
	    (uintptr_t)(bin_info->reg_size * regind));
	slab_data->nfree--;
	return (ret);
}

JEMALLOC_INLINE_C size_t
arena_slab_regind(extent_t *slab, const arena_bin_info_t *bin_info,
    const void *ptr)
{
	size_t diff, interval, shift, regind;

	/* Freeing a pointer outside the slab can cause assertion failure. */
	assert((uintptr_t)ptr >= (uintptr_t)extent_addr_get(slab));
	assert((uintptr_t)ptr < (uintptr_t)extent_past_get(slab));
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab)) %
	    (uintptr_t)bin_info->reg_size == 0);

	/*
	 * Avoid doing division with a variable divisor if possible.  Using
	 * actual division here can reduce allocator throughput by over 20%!
	 */
	diff = (size_t)((uintptr_t)ptr - (uintptr_t)extent_addr_get(slab));

	/* Rescale (factor powers of 2 out of the numerator and denominator). */
	interval = bin_info->reg_size;
	shift = ffs_zu(interval) - 1;
	diff >>= shift;
	interval >>= shift;

	if (interval == 1) {
		/* The divisor was a power of 2. */
		regind = diff;
	} else {
		/*
		 * To divide by a number D that is not a power of two we
		 * multiply by (2^21 / D) and then right shift by 21 positions.
		 *
		 *   X / D
		 *
		 * becomes
		 *
		 *   (X * interval_invs[D - 3]) >> SIZE_INV_SHIFT
		 *
		 * We can omit the first three elements, because we never
		 * divide by 0, and 1 and 2 are both powers of two, which are
		 * handled above.
		 */
#define	SIZE_INV_SHIFT	((sizeof(size_t) << 3) - LG_SLAB_MAXREGS)
#define	SIZE_INV(s)	(((ZU(1) << SIZE_INV_SHIFT) / (s)) + 1)
		static const size_t interval_invs[] = {
		    SIZE_INV(3),
		    SIZE_INV(4), SIZE_INV(5), SIZE_INV(6), SIZE_INV(7),
		    SIZE_INV(8), SIZE_INV(9), SIZE_INV(10), SIZE_INV(11),
		    SIZE_INV(12), SIZE_INV(13), SIZE_INV(14), SIZE_INV(15),
		    SIZE_INV(16), SIZE_INV(17), SIZE_INV(18), SIZE_INV(19),
		    SIZE_INV(20), SIZE_INV(21), SIZE_INV(22), SIZE_INV(23),
		    SIZE_INV(24), SIZE_INV(25), SIZE_INV(26), SIZE_INV(27),
		    SIZE_INV(28), SIZE_INV(29), SIZE_INV(30), SIZE_INV(31)
		};

		if (likely(interval <= ((sizeof(interval_invs) / sizeof(size_t))
		    + 2))) {
			regind = (diff * interval_invs[interval - 3]) >>
			    SIZE_INV_SHIFT;
		} else
			regind = diff / interval;
#undef SIZE_INV
#undef SIZE_INV_SHIFT
	}
	assert(diff == regind * interval);
	assert(regind < bin_info->nregs);

	return (regind);
}

JEMALLOC_INLINE_C void
arena_slab_reg_dalloc(tsdn_t *tsdn, extent_t *slab,
    arena_slab_data_t *slab_data, void *ptr)
{
	szind_t binind = slab_data->binind;
	const arena_bin_info_t *bin_info = &arena_bin_info[binind];
	size_t regind = arena_slab_regind(slab, bin_info, ptr);

	assert(slab_data->nfree < bin_info->nregs);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(slab_data->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(slab_data->bitmap, &bin_info->bitmap_info, regind);
	slab_data->nfree++;
}

static void
arena_nactive_add(arena_t *arena, size_t add_pages)
{

	arena->nactive += add_pages;
}

static void
arena_nactive_sub(arena_t *arena, size_t sub_pages)
{

	assert(arena->nactive >= sub_pages);
	arena->nactive -= sub_pages;
}

static void
arena_large_malloc_stats_update(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize);
	szind_t hindex = (index >= NBINS) ? index - NBINS : 0;

	cassert(config_stats);

	arena->stats.nmalloc_large++;
	arena->stats.allocated_large += usize;
	arena->stats.lstats[hindex].nmalloc++;
	arena->stats.lstats[hindex].nrequests++;
	arena->stats.lstats[hindex].curlextents++;
}

static void
arena_large_malloc_stats_update_undo(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize);
	szind_t hindex = (index >= NBINS) ? index - NBINS : 0;

	cassert(config_stats);

	arena->stats.nmalloc_large--;
	arena->stats.allocated_large -= usize;
	arena->stats.lstats[hindex].nmalloc--;
	arena->stats.lstats[hindex].nrequests--;
	arena->stats.lstats[hindex].curlextents--;
}

static void
arena_large_dalloc_stats_update(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize);
	szind_t hindex = (index >= NBINS) ? index - NBINS : 0;

	cassert(config_stats);

	arena->stats.ndalloc_large++;
	arena->stats.allocated_large -= usize;
	arena->stats.lstats[hindex].ndalloc++;
	arena->stats.lstats[hindex].curlextents--;
}

static void
arena_large_reset_stats_cancel(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize);
	szind_t hindex = (index >= NBINS) ? index - NBINS : 0;

	cassert(config_stats);

	arena->stats.ndalloc_large++;
	arena->stats.lstats[hindex].ndalloc--;
}

static void
arena_large_ralloc_stats_update(arena_t *arena, size_t oldusize, size_t usize)
{

	arena_large_dalloc_stats_update(arena, oldusize);
	arena_large_malloc_stats_update(arena, usize);
}

static extent_t *
arena_extent_alloc_large_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, size_t usize, size_t alignment, bool *zero)
{
	extent_t *extent;
	bool commit = true;

	extent = extent_alloc_wrapper(tsdn, arena, r_extent_hooks, NULL, usize,
	    large_pad, alignment, zero, &commit, false);
	if (extent == NULL) {
		/* Revert optimistic stats updates. */
		malloc_mutex_lock(tsdn, &arena->lock);
		if (config_stats) {
			arena_large_malloc_stats_update_undo(arena, usize);
			arena->stats.mapped -= usize;
		}
		arena_nactive_sub(arena, (usize + large_pad) >> LG_PAGE);
		malloc_mutex_unlock(tsdn, &arena->lock);
	}

	return (extent);
}

extent_t *
arena_extent_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool *zero)
{
	extent_t *extent;
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	malloc_mutex_lock(tsdn, &arena->lock);

	/* Optimistically update stats. */
	if (config_stats) {
		arena_large_malloc_stats_update(arena, usize);
		arena->stats.mapped += usize;
	}
	arena_nactive_add(arena, (usize + large_pad) >> LG_PAGE);

	extent = arena_extent_cache_alloc_locked(tsdn, arena, &extent_hooks,
	    NULL, usize, large_pad, alignment, zero, false);
	malloc_mutex_unlock(tsdn, &arena->lock);
	if (extent == NULL) {
		extent = arena_extent_alloc_large_hard(tsdn, arena,
		    &extent_hooks, usize, alignment, zero);
	}

	return (extent);
}

void
arena_extent_dalloc_large(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    bool locked)
{
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	if (!locked)
		malloc_mutex_lock(tsdn, &arena->lock);
	else
		malloc_mutex_assert_owner(tsdn, &arena->lock);
	if (config_stats) {
		arena_large_dalloc_stats_update(arena,
		    extent_usize_get(extent));
		arena->stats.mapped -= extent_size_get(extent);
	}
	arena_nactive_sub(arena, extent_size_get(extent) >> LG_PAGE);

	arena_extent_cache_dalloc_locked(tsdn, arena, &extent_hooks, extent);
	if (!locked)
		malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_extent_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize)
{
	size_t usize = extent_usize_get(extent);
	size_t udiff = oldusize - usize;

	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_stats) {
		arena_large_ralloc_stats_update(arena, oldusize, usize);
		arena->stats.mapped -= udiff;
	}
	arena_nactive_sub(arena, udiff >> LG_PAGE);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_extent_ralloc_large_expand(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldusize)
{
	size_t usize = extent_usize_get(extent);
	size_t udiff = usize - oldusize;

	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_stats) {
		arena_large_ralloc_stats_update(arena, oldusize, usize);
		arena->stats.mapped += udiff;
	}
	arena_nactive_add(arena, udiff >> LG_PAGE);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

static void
arena_decay_deadline_init(arena_t *arena)
{

	/*
	 * Generate a new deadline that is uniformly random within the next
	 * epoch after the current one.
	 */
	nstime_copy(&arena->decay.deadline, &arena->decay.epoch);
	nstime_add(&arena->decay.deadline, &arena->decay.interval);
	if (arena->decay.time > 0) {
		nstime_t jitter;

		nstime_init(&jitter, prng_range(&arena->decay.jitter_state,
		    nstime_ns(&arena->decay.interval), false));
		nstime_add(&arena->decay.deadline, &jitter);
	}
}

static bool
arena_decay_deadline_reached(const arena_t *arena, const nstime_t *time)
{

	return (nstime_compare(&arena->decay.deadline, time) <= 0);
}

static size_t
arena_decay_backlog_npages_limit(const arena_t *arena)
{
	static const uint64_t h_steps[] = {
#define	STEP(step, h, x, y) \
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
	for (i = 0; i < SMOOTHSTEP_NSTEPS; i++)
		sum += arena->decay.backlog[i] * h_steps[i];
	npages_limit_backlog = (size_t)(sum >> SMOOTHSTEP_BFP);

	return (npages_limit_backlog);
}

static void
arena_decay_backlog_update_last(arena_t *arena)
{
	size_t ndirty_delta = (arena->ndirty > arena->decay.ndirty) ?
	    arena->ndirty - arena->decay.ndirty : 0;
	arena->decay.backlog[SMOOTHSTEP_NSTEPS-1] = ndirty_delta;
}

static void
arena_decay_backlog_update(arena_t *arena, uint64_t nadvance_u64)
{

	if (nadvance_u64 >= SMOOTHSTEP_NSTEPS) {
		memset(arena->decay.backlog, 0, (SMOOTHSTEP_NSTEPS-1) *
		    sizeof(size_t));
	} else {
		size_t nadvance_z = (size_t)nadvance_u64;

		assert((uint64_t)nadvance_z == nadvance_u64);

		memmove(arena->decay.backlog, &arena->decay.backlog[nadvance_z],
		    (SMOOTHSTEP_NSTEPS - nadvance_z) * sizeof(size_t));
		if (nadvance_z > 1) {
			memset(&arena->decay.backlog[SMOOTHSTEP_NSTEPS -
			    nadvance_z], 0, (nadvance_z-1) * sizeof(size_t));
		}
	}

	arena_decay_backlog_update_last(arena);
}

static void
arena_decay_epoch_advance_helper(arena_t *arena, const nstime_t *time)
{
	uint64_t nadvance_u64;
	nstime_t delta;

	assert(arena_decay_deadline_reached(arena, time));

	nstime_copy(&delta, time);
	nstime_subtract(&delta, &arena->decay.epoch);
	nadvance_u64 = nstime_divide(&delta, &arena->decay.interval);
	assert(nadvance_u64 > 0);

	/* Add nadvance_u64 decay intervals to epoch. */
	nstime_copy(&delta, &arena->decay.interval);
	nstime_imultiply(&delta, nadvance_u64);
	nstime_add(&arena->decay.epoch, &delta);

	/* Set a new deadline. */
	arena_decay_deadline_init(arena);

	/* Update the backlog. */
	arena_decay_backlog_update(arena, nadvance_u64);
}

static void
arena_decay_epoch_advance_purge(tsdn_t *tsdn, arena_t *arena)
{
	size_t ndirty_limit = arena_decay_backlog_npages_limit(arena);

	if (arena->ndirty > ndirty_limit)
		arena_purge_to_limit(tsdn, arena, ndirty_limit);
	arena->decay.ndirty = arena->ndirty;
}

static void
arena_decay_epoch_advance(tsdn_t *tsdn, arena_t *arena, const nstime_t *time)
{

	arena_decay_epoch_advance_helper(arena, time);
	arena_decay_epoch_advance_purge(tsdn, arena);
}

static void
arena_decay_init(arena_t *arena, ssize_t decay_time)
{

	arena->decay.time = decay_time;
	if (decay_time > 0) {
		nstime_init2(&arena->decay.interval, decay_time, 0);
		nstime_idivide(&arena->decay.interval, SMOOTHSTEP_NSTEPS);
	}

	nstime_init(&arena->decay.epoch, 0);
	nstime_update(&arena->decay.epoch);
	arena->decay.jitter_state = (uint64_t)(uintptr_t)arena;
	arena_decay_deadline_init(arena);
	arena->decay.ndirty = arena->ndirty;
	memset(arena->decay.backlog, 0, SMOOTHSTEP_NSTEPS * sizeof(size_t));
}

static bool
arena_decay_time_valid(ssize_t decay_time)
{

	if (decay_time < -1)
		return (false);
	if (decay_time == -1 || (uint64_t)decay_time <= NSTIME_SEC_MAX)
		return (true);
	return (false);
}

ssize_t
arena_decay_time_get(tsdn_t *tsdn, arena_t *arena)
{
	ssize_t decay_time;

	malloc_mutex_lock(tsdn, &arena->lock);
	decay_time = arena->decay.time;
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (decay_time);
}

bool
arena_decay_time_set(tsdn_t *tsdn, arena_t *arena, ssize_t decay_time)
{

	if (!arena_decay_time_valid(decay_time))
		return (true);

	malloc_mutex_lock(tsdn, &arena->lock);
	/*
	 * Restart decay backlog from scratch, which may cause many dirty pages
	 * to be immediately purged.  It would conceptually be possible to map
	 * the old backlog onto the new backlog, but there is no justification
	 * for such complexity since decay_time changes are intended to be
	 * infrequent, either between the {-1, 0, >0} states, or a one-time
	 * arbitrary change during initial arena configuration.
	 */
	arena_decay_init(arena, decay_time);
	arena_maybe_purge(tsdn, arena);
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (false);
}

static void
arena_maybe_purge_helper(tsdn_t *tsdn, arena_t *arena)
{
	nstime_t time;

	/* Purge all or nothing if the option is disabled. */
	if (arena->decay.time <= 0) {
		if (arena->decay.time == 0)
			arena_purge_to_limit(tsdn, arena, 0);
		return;
	}

	nstime_init(&time, 0);
	nstime_update(&time);
	if (unlikely(!nstime_monotonic() && nstime_compare(&arena->decay.epoch,
	    &time) > 0)) {
		/*
		 * Time went backwards.  Move the epoch back in time and
		 * generate a new deadline, with the expectation that time
		 * typically flows forward for long enough periods of time that
		 * epochs complete.  Unfortunately, this strategy is susceptible
		 * to clock jitter triggering premature epoch advances, but
		 * clock jitter estimation and compensation isn't feasible here
		 * because calls into this code are event-driven.
		 */
		nstime_copy(&arena->decay.epoch, &time);
		arena_decay_deadline_init(arena);
	} else {
		/* Verify that time does not go backwards. */
		assert(nstime_compare(&arena->decay.epoch, &time) <= 0);
	}

	/*
	 * If the deadline has been reached, advance to the current epoch and
	 * purge to the new limit if necessary.  Note that dirty pages created
	 * during the current epoch are not subject to purge until a future
	 * epoch, so as a result purging only happens during epoch advances.
	 */
	if (arena_decay_deadline_reached(arena, &time))
		arena_decay_epoch_advance(tsdn, arena, &time);
}

void
arena_maybe_purge(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	/* Don't recursively purge. */
	if (arena->purging)
		return;

	arena_maybe_purge_helper(tsdn, arena);
}

static size_t
arena_dirty_count(tsdn_t *tsdn, arena_t *arena)
{
	extent_t *extent;
	size_t ndirty = 0;

	malloc_mutex_assert_owner(tsdn, &arena->extents_mtx);

	for (extent = qr_next(&arena->extents_dirty, qr_link); extent !=
	    &arena->extents_dirty; extent = qr_next(extent, qr_link))
		ndirty += extent_size_get(extent) >> LG_PAGE;

	return (ndirty);
}

static size_t
arena_stash_dirty(tsdn_t *tsdn, arena_t *arena, extent_hooks_t **r_extent_hooks,
    size_t ndirty_limit, extent_t *purge_extents_sentinel)
{
	extent_t *extent, *next;
	size_t nstashed = 0;

	malloc_mutex_lock(tsdn, &arena->extents_mtx);

	/* Stash extents according to ndirty_limit. */
	for (extent = qr_next(&arena->extents_dirty, qr_link); extent !=
	    &arena->extents_dirty; extent = next) {
		size_t npages;
		bool zero;
		UNUSED extent_t *textent;

		npages = extent_size_get(extent) >> LG_PAGE;
		if (arena->ndirty - (nstashed + npages) < ndirty_limit)
			break;

		next = qr_next(extent, qr_link);
		/* Allocate. */
		zero = false;
		textent = extent_alloc_cache_locked(tsdn, arena, r_extent_hooks,
		    extent_base_get(extent), extent_size_get(extent), 0, PAGE,
		    &zero, false);
		assert(textent == extent);
		assert(zero == extent_zeroed_get(extent));
		extent_ring_remove(extent);
		extent_ring_insert(purge_extents_sentinel, extent);

		nstashed += npages;
	}

	malloc_mutex_unlock(tsdn, &arena->extents_mtx);
	return (nstashed);
}

static size_t
arena_purge_stashed(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *purge_extents_sentinel)
{
	UNUSED size_t nmadvise;
	size_t npurged;
	extent_t *extent, *next;

	if (config_stats)
		nmadvise = 0;
	npurged = 0;

	for (extent = qr_next(purge_extents_sentinel, qr_link); extent !=
	    purge_extents_sentinel; extent = next) {
		if (config_stats)
			nmadvise++;
		npurged += extent_size_get(extent) >> LG_PAGE;

		next = qr_next(extent, qr_link);
		extent_ring_remove(extent);
		extent_dalloc_wrapper(tsdn, arena, r_extent_hooks, extent);
	}

	if (config_stats) {
		arena->stats.nmadvise += nmadvise;
		arena->stats.purged += npurged;
	}

	return (npurged);
}

/*
 *   ndirty_limit: Purge as many dirty extents as possible without violating the
 *   invariant: (arena->ndirty >= ndirty_limit)
 */
static void
arena_purge_to_limit(tsdn_t *tsdn, arena_t *arena, size_t ndirty_limit)
{
	extent_hooks_t *extent_hooks = extent_hooks_get(arena);
	size_t npurge, npurged;
	extent_t purge_extents_sentinel;

	arena->purging = true;

	/*
	 * Calls to arena_dirty_count() are disabled even for debug builds
	 * because overhead grows nonlinearly as memory usage increases.
	 */
	if (false && config_debug) {
		size_t ndirty = arena_dirty_count(tsdn, arena);
		assert(ndirty == arena->ndirty);
	}
	extent_init(&purge_extents_sentinel, arena, NULL, 0, 0, false, false,
	    false, false);

	npurge = arena_stash_dirty(tsdn, arena, &extent_hooks, ndirty_limit,
	    &purge_extents_sentinel);
	if (npurge == 0)
		goto label_return;
	npurged = arena_purge_stashed(tsdn, arena, &extent_hooks,
	    &purge_extents_sentinel);
	assert(npurged == npurge);

	if (config_stats)
		arena->stats.npurge++;

label_return:
	arena->purging = false;
}

void
arena_purge(tsdn_t *tsdn, arena_t *arena, bool all)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	if (all)
		arena_purge_to_limit(tsdn, arena, 0);
	else
		arena_maybe_purge(tsdn, arena);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

static void
arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *slab)
{
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;

	arena_nactive_sub(arena, extent_size_get(slab) >> LG_PAGE);
	arena_extent_cache_dalloc_locked(tsdn, arena, &extent_hooks, slab);
}

void
arena_reset(tsd_t *tsd, arena_t *arena)
{
	unsigned i;
	extent_t *extent;

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
	for (extent = ql_last(&arena->large, ql_link); extent != NULL; extent =
	    ql_last(&arena->large, ql_link)) {
		void *ptr = extent_base_get(extent);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);
		if (config_stats || (config_prof && opt_prof))
			usize = isalloc(tsd_tsdn(tsd), extent, ptr);
		/* Remove large allocation from prof sample set. */
		if (config_prof && opt_prof)
			prof_free(tsd, extent, ptr, usize);
		large_dalloc(tsd_tsdn(tsd), extent);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->large_mtx);
		/* Cancel out unwanted effects on stats. */
		if (config_stats)
			arena_large_reset_stats_cancel(arena, usize);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->large_mtx);

	malloc_mutex_lock(tsd_tsdn(tsd), &arena->lock);

	/* Bins. */
	for (i = 0; i < NBINS; i++) {
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
		for (slab = qr_next(&bin->slabs_full, qr_link); slab !=
		    &bin->slabs_full; slab = qr_next(&bin->slabs_full,
		    qr_link)) {
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

	assert(!arena->purging);
	arena->nactive = 0;

	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->lock);
}

static void
arena_bin_slabs_nonfull_insert(arena_bin_t *bin, extent_t *slab)
{

	assert(extent_slab_data_get(slab)->nfree > 0);
	extent_heap_insert(&bin->slabs_nonfull, slab);
}

static void
arena_bin_slabs_nonfull_remove(arena_bin_t *bin, extent_t *slab)
{

	extent_heap_remove(&bin->slabs_nonfull, slab);
}

static extent_t *
arena_bin_slabs_nonfull_tryget(arena_bin_t *bin)
{
	extent_t *slab = extent_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL)
		return (NULL);
	if (config_stats)
		bin->stats.reslabs++;
	return (slab);
}

static void
arena_bin_slabs_full_insert(arena_bin_t *bin, extent_t *slab)
{

	assert(extent_slab_data_get(slab)->nfree == 0);
	extent_ring_insert(&bin->slabs_full, slab);
}

static void
arena_bin_slabs_full_remove(extent_t *slab)
{

	extent_ring_remove(slab);
}

static extent_t *
arena_slab_alloc_hard(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, const arena_bin_info_t *bin_info)
{
	extent_t *slab;
	bool zero, commit;

	zero = false;
	commit = true;
	malloc_mutex_unlock(tsdn, &arena->lock);
	slab = extent_alloc_wrapper(tsdn, arena, r_extent_hooks, NULL,
	    bin_info->slab_size, 0, PAGE, &zero, &commit, true);
	malloc_mutex_lock(tsdn, &arena->lock);

	return (slab);
}

static extent_t *
arena_slab_alloc(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    const arena_bin_info_t *bin_info)
{
	extent_t *slab;
	arena_slab_data_t *slab_data;
	extent_hooks_t *extent_hooks = EXTENT_HOOKS_INITIALIZER;
	bool zero;

	zero = false;
	slab = arena_extent_cache_alloc_locked(tsdn, arena, &extent_hooks, NULL,
	    bin_info->slab_size, 0, PAGE, &zero, true);
	if (slab == NULL) {
		slab = arena_slab_alloc_hard(tsdn, arena, &extent_hooks,
		    bin_info);
		if (slab == NULL)
			return (NULL);
	}
	assert(extent_slab_get(slab));

	arena_nactive_add(arena, extent_size_get(slab) >> LG_PAGE);

	/* Initialize slab internals. */
	slab_data = extent_slab_data_get(slab);
	slab_data->binind = binind;
	slab_data->nfree = bin_info->nregs;
	bitmap_init(slab_data->bitmap, &bin_info->bitmap_info);

	if (config_stats)
		arena->stats.mapped += extent_size_get(slab);

	return (slab);
}

static extent_t *
arena_bin_nonfull_slab_get(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin,
    szind_t binind)
{
	extent_t *slab;
	const arena_bin_info_t *bin_info;

	/* Look for a usable slab. */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL)
		return (slab);
	/* No existing slabs have any space available. */

	bin_info = &arena_bin_info[binind];

	/* Allocate a new slab. */
	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	malloc_mutex_lock(tsdn, &arena->lock);
	slab = arena_slab_alloc(tsdn, arena, binind, bin_info);
	malloc_mutex_unlock(tsdn, &arena->lock);
	/********************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (slab != NULL) {
		if (config_stats) {
			bin->stats.nslabs++;
			bin->stats.curslabs++;
		}
		return (slab);
	}

	/*
	 * arena_slab_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	slab = arena_bin_slabs_nonfull_tryget(bin);
	if (slab != NULL)
		return (slab);

	return (NULL);
}

/* Re-fill bin->slabcur, then call arena_slab_reg_alloc(). */
static void *
arena_bin_malloc_hard(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin,
    szind_t binind)
{
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
			return (ret);
		}

		arena_bin_slabs_full_insert(bin, bin->slabcur);
		bin->slabcur = NULL;
	}

	if (slab == NULL)
		return (NULL);
	bin->slabcur = slab;

	assert(extent_slab_data_get(bin->slabcur)->nfree > 0);

	return (arena_slab_reg_alloc(tsdn, slab, bin_info));
}

void
arena_tcache_fill_small(tsdn_t *tsdn, arena_t *arena, tcache_bin_t *tbin,
    szind_t binind, uint64_t prof_accumbytes)
{
	unsigned i, nfill;
	arena_bin_t *bin;

	assert(tbin->ncached == 0);

	if (config_prof && arena_prof_accum(tsdn, arena, prof_accumbytes))
		prof_idump(tsdn);
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
		} else
			ptr = arena_bin_malloc_hard(tsdn, arena, bin, binind);
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
arena_alloc_junk_small(void *ptr, const arena_bin_info_t *bin_info, bool zero)
{

	if (!zero)
		memset(ptr, JEMALLOC_ALLOC_JUNK, bin_info->reg_size);
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(n_arena_dalloc_junk_small)
#endif
void
arena_dalloc_junk_small(void *ptr, const arena_bin_info_t *bin_info)
{

	memset(ptr, JEMALLOC_FREE_JUNK, bin_info->reg_size);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small)
arena_dalloc_junk_small_t *arena_dalloc_junk_small =
    JEMALLOC_N(n_arena_dalloc_junk_small);
#endif

static void *
arena_malloc_small(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	size_t usize;
	extent_t *slab;

	assert(binind < NBINS);
	bin = &arena->bins[binind];
	usize = index2size(binind);

	malloc_mutex_lock(tsdn, &bin->lock);
	if ((slab = bin->slabcur) != NULL && extent_slab_data_get(slab)->nfree >
	    0)
		ret = arena_slab_reg_alloc(tsdn, slab, &arena_bin_info[binind]);
	else
		ret = arena_bin_malloc_hard(tsdn, arena, bin, binind);

	if (ret == NULL) {
		malloc_mutex_unlock(tsdn, &bin->lock);
		return (NULL);
	}

	if (config_stats) {
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
		bin->stats.curregs++;
	}
	malloc_mutex_unlock(tsdn, &bin->lock);
	if (config_prof && arena_prof_accum(tsdn, arena, usize))
		prof_idump(tsdn);

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc)) {
				arena_alloc_junk_small(ret,
				    &arena_bin_info[binind], false);
			} else if (unlikely(opt_zero))
				memset(ret, 0, usize);
		}
	} else {
		if (config_fill && unlikely(opt_junk_alloc)) {
			arena_alloc_junk_small(ret, &arena_bin_info[binind],
			    true);
		}
		memset(ret, 0, usize);
	}

	arena_decay_tick(tsdn, arena);
	return (ret);
}

void *
arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero)
{

	assert(!tsdn_null(tsdn) || arena != NULL);

	if (likely(!tsdn_null(tsdn)))
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	if (unlikely(arena == NULL))
		return (NULL);

	if (likely(size <= SMALL_MAXCLASS))
		return (arena_malloc_small(tsdn, arena, ind, zero));
	return (large_malloc(tsdn, arena, index2size(ind), zero));
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache)
{
	void *ret;

	if (usize <= SMALL_MAXCLASS && (alignment < PAGE || (alignment == PAGE
	    && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special slab placement. */
		ret = arena_malloc(tsdn, arena, usize, size2index(usize), zero,
		    tcache, true);
	} else {
		if (likely(alignment <= CACHELINE))
			ret = large_malloc(tsdn, arena, usize, zero);
		else
			ret = large_palloc(tsdn, arena, usize, alignment, zero);
	}
	return (ret);
}

void
arena_prof_promote(tsdn_t *tsdn, extent_t *extent, const void *ptr,
    size_t usize)
{
	arena_t *arena = extent_arena_get(extent);

	cassert(config_prof);
	assert(ptr != NULL);
	assert(isalloc(tsdn, extent, ptr) == LARGE_MINCLASS);
	assert(usize <= SMALL_MAXCLASS);

	extent_usize_set(extent, usize);

	/*
	 * Cancel out as much of the excessive prof_accumbytes increase as
	 * possible without underflowing.  Interval-triggered dumps occur
	 * slightly more often than intended as a result of incomplete
	 * canceling.
	 */
	malloc_mutex_lock(tsdn, &arena->lock);
	if (arena->prof_accumbytes >= LARGE_MINCLASS - usize)
		arena->prof_accumbytes -= LARGE_MINCLASS - usize;
	else
		arena->prof_accumbytes = 0;
	malloc_mutex_unlock(tsdn, &arena->lock);

	assert(isalloc(tsdn, extent, ptr) == usize);
}

static size_t
arena_prof_demote(tsdn_t *tsdn, extent_t *extent, const void *ptr)
{

	cassert(config_prof);
	assert(ptr != NULL);

	extent_usize_set(extent, LARGE_MINCLASS);

	assert(isalloc(tsdn, extent, ptr) == LARGE_MINCLASS);

	return (LARGE_MINCLASS);
}

void
arena_dalloc_promoted(tsdn_t *tsdn, extent_t *extent, void *ptr,
    tcache_t *tcache, bool slow_path)
{
	size_t usize;

	cassert(config_prof);
	assert(opt_prof);

	usize = arena_prof_demote(tsdn, extent, ptr);
	if (usize <= tcache_maxclass) {
		tcache_dalloc_large(tsdn_tsd(tsdn), tcache, ptr, usize,
		    slow_path);
	} else
		large_dalloc(tsdn, extent);
}

static void
arena_dissociate_bin_slab(extent_t *slab, arena_bin_t *bin)
{

	/* Dissociate slab from bin. */
	if (slab == bin->slabcur)
		bin->slabcur = NULL;
	else {
		szind_t binind = extent_slab_data_get(slab)->binind;
		const arena_bin_info_t *bin_info = &arena_bin_info[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1)
			arena_bin_slabs_full_remove(slab);
		else
			arena_bin_slabs_nonfull_remove(bin, slab);
	}
}

static void
arena_dalloc_bin_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin)
{

	assert(slab != bin->slabcur);

	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	malloc_mutex_lock(tsdn, &arena->lock);
	arena_slab_dalloc(tsdn, arena, slab);
	malloc_mutex_unlock(tsdn, &arena->lock);
	/****************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats)
		bin->stats.curslabs--;
}

static void
arena_bin_lower_slab(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    arena_bin_t *bin)
{

	assert(extent_slab_data_get(slab)->nfree > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the lowest
	 * non-full slab.  It is okay to NULL slabcur out rather than
	 * proactively keeping it pointing at the lowest non-full slab.
	 */
	if (bin->slabcur != NULL && (uintptr_t)extent_addr_get(slab) <
	    (uintptr_t)extent_addr_get(bin->slabcur)) {
		/* Switch slabcur. */
		if (extent_slab_data_get(bin->slabcur)->nfree > 0)
			arena_bin_slabs_nonfull_insert(bin, bin->slabcur);
		else
			arena_bin_slabs_full_insert(bin, bin->slabcur);
		bin->slabcur = slab;
		if (config_stats)
			bin->stats.reslabs++;
	} else
		arena_bin_slabs_nonfull_insert(bin, slab);
}

static void
arena_dalloc_bin_locked_impl(tsdn_t *tsdn, arena_t *arena, extent_t *slab,
    void *ptr, bool junked)
{
	arena_slab_data_t *slab_data = extent_slab_data_get(slab);
	szind_t binind = slab_data->binind;
	arena_bin_t *bin = &arena->bins[binind];
	const arena_bin_info_t *bin_info = &arena_bin_info[binind];

	if (!junked && config_fill && unlikely(opt_junk_free))
		arena_dalloc_junk_small(ptr, bin_info);

	arena_slab_reg_dalloc(tsdn, slab, slab_data, ptr);
	if (slab_data->nfree == bin_info->nregs) {
		arena_dissociate_bin_slab(slab, bin);
		arena_dalloc_bin_slab(tsdn, arena, slab, bin);
	} else if (slab_data->nfree == 1 && slab != bin->slabcur) {
		arena_bin_slabs_full_remove(slab);
		arena_bin_lower_slab(tsdn, arena, slab, bin);
	}

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}
}

void
arena_dalloc_bin_junked_locked(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    void *ptr)
{

	arena_dalloc_bin_locked_impl(tsdn, arena, extent, ptr, true);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr)
{
	arena_bin_t *bin = &arena->bins[extent_slab_data_get(extent)->binind];

	malloc_mutex_lock(tsdn, &bin->lock);
	arena_dalloc_bin_locked_impl(tsdn, arena, extent, ptr, false);
	malloc_mutex_unlock(tsdn, &bin->lock);
}

void
arena_dalloc_small(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr)
{

	arena_dalloc_bin(tsdn, arena, extent, ptr);
	arena_decay_tick(tsdn, arena);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, extent_t *extent, void *ptr, size_t oldsize,
    size_t size, size_t extra, bool zero)
{
	size_t usize_min, usize_max;

	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= LARGE_MAXCLASS);

	if (unlikely(size > LARGE_MAXCLASS))
		return (true);

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
		    oldsize))
			return (true);

		arena_decay_tick(tsdn, extent_arena_get(extent));
		return (false);
	} else if (oldsize >= LARGE_MINCLASS && usize_max >= LARGE_MINCLASS) {
		return (large_ralloc_no_move(tsdn, extent, usize_min, usize_max,
		    zero));
	}

	return (true);
}

static void *
arena_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache)
{

	if (alignment == 0)
		return (arena_malloc(tsdn, arena, usize, size2index(usize),
		    zero, tcache, true));
	usize = sa2u(usize, alignment);
	if (unlikely(usize == 0 || usize > LARGE_MAXCLASS))
		return (NULL);
	return (ipalloct(tsdn, usize, alignment, zero, tcache, arena));
}

void *
arena_ralloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent, void *ptr,
    size_t oldsize, size_t size, size_t alignment, bool zero, tcache_t *tcache)
{
	void *ret;
	size_t usize, copysize;

	usize = s2u(size);
	if (unlikely(usize == 0 || size > LARGE_MAXCLASS))
		return (NULL);

	if (likely(usize <= SMALL_MAXCLASS)) {
		/* Try to avoid moving the allocation. */
		if (!arena_ralloc_no_move(tsdn, extent, ptr, oldsize, usize, 0,
		    zero))
			return (ptr);
	}

	if (oldsize >= LARGE_MINCLASS && usize >= LARGE_MINCLASS) {
		return (large_ralloc(tsdn, arena, extent, usize, alignment,
		    zero, tcache));
	}

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and copying.
	 */
	ret = arena_ralloc_move_helper(tsdn, arena, usize, alignment, zero,
	    tcache);
	if (ret == NULL)
		return (NULL);

	/*
	 * Junk/zero-filling were already done by
	 * ipalloc()/arena_malloc().
	 */

	copysize = (usize < oldsize) ? usize : oldsize;
	memcpy(ret, ptr, copysize);
	isdalloct(tsdn, extent, ptr, oldsize, tcache, true);
	return (ret);
}

dss_prec_t
arena_dss_prec_get(tsdn_t *tsdn, arena_t *arena)
{
	dss_prec_t ret;

	malloc_mutex_lock(tsdn, &arena->lock);
	ret = arena->dss_prec;
	malloc_mutex_unlock(tsdn, &arena->lock);
	return (ret);
}

bool
arena_dss_prec_set(tsdn_t *tsdn, arena_t *arena, dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	malloc_mutex_lock(tsdn, &arena->lock);
	arena->dss_prec = dss_prec;
	malloc_mutex_unlock(tsdn, &arena->lock);
	return (false);
}

ssize_t
arena_decay_time_default_get(void)
{

	return ((ssize_t)atomic_read_z((size_t *)&decay_time_default));
}

bool
arena_decay_time_default_set(ssize_t decay_time)
{

	if (!arena_decay_time_valid(decay_time))
		return (true);
	atomic_write_z((size_t *)&decay_time_default, (size_t)decay_time);
	return (false);
}

static void
arena_basic_stats_merge_locked(arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *decay_time, size_t *nactive, size_t *ndirty)
{

	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena->dss_prec];
	*decay_time = arena->decay.time;
	*nactive += arena->nactive;
	*ndirty += arena->ndirty;
}

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *decay_time, size_t *nactive, size_t *ndirty)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_basic_stats_merge_locked(arena, nthreads, dss, decay_time,
	    nactive, ndirty);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *decay_time, size_t *nactive, size_t *ndirty,
    arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats)
{
	unsigned i;

	cassert(config_stats);

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_basic_stats_merge_locked(arena, nthreads, dss, decay_time,
	    nactive, ndirty);

	astats->mapped += arena->stats.mapped;
	astats->retained += arena->stats.retained;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->metadata += arena_metadata_get(arena);
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;
	astats->nrequests_large += arena->stats.nrequests_large;

	for (i = 0; i < NSIZES - NBINS; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].curlextents += arena->stats.lstats[i].curlextents;
	}
	malloc_mutex_unlock(tsdn, &arena->lock);

	for (i = 0; i < NBINS; i++) {
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

unsigned
arena_nthreads_get(arena_t *arena, bool internal)
{

	return (atomic_read_u(&arena->nthreads[internal]));
}

void
arena_nthreads_inc(arena_t *arena, bool internal)
{

	atomic_add_u(&arena->nthreads[internal], 1);
}

void
arena_nthreads_dec(arena_t *arena, bool internal)
{

	atomic_sub_u(&arena->nthreads[internal], 1);
}

arena_t *
arena_new(tsdn_t *tsdn, unsigned ind)
{
	arena_t *arena;
	unsigned i;

	arena = (arena_t *)base_alloc(tsdn, sizeof(arena_t));
	if (arena == NULL)
		return (NULL);

	arena->ind = ind;
	arena->nthreads[0] = arena->nthreads[1] = 0;
	if (malloc_mutex_init(&arena->lock, "arena", WITNESS_RANK_ARENA))
		return (NULL);

	if (config_stats && config_tcache)
		ql_new(&arena->tcache_ql);

	if (config_prof)
		arena->prof_accumbytes = 0;

	if (config_cache_oblivious) {
		/*
		 * A nondeterministic seed based on the address of arena reduces
		 * the likelihood of lockstep non-uniform cache index
		 * utilization among identical concurrent processes, but at the
		 * cost of test repeatability.  For debug builds, instead use a
		 * deterministic seed.
		 */
		arena->offset_state = config_debug ? ind :
		    (uint64_t)(uintptr_t)arena;
	}

	arena->dss_prec = extent_dss_prec_get();

	arena->purging = false;
	arena->nactive = 0;
	arena->ndirty = 0;

	arena_decay_init(arena, arena_decay_time_default_get());

	ql_new(&arena->large);
	if (malloc_mutex_init(&arena->large_mtx, "arena_large",
	    WITNESS_RANK_ARENA_LARGE))
		return (NULL);

	for (i = 0; i < NPSIZES; i++) {
		extent_heap_new(&arena->extents_cached[i]);
		extent_heap_new(&arena->extents_retained[i]);
	}

	extent_init(&arena->extents_dirty, arena, NULL, 0, 0, false, false,
	    false, false);

	if (malloc_mutex_init(&arena->extents_mtx, "arena_extents",
	    WITNESS_RANK_ARENA_EXTENTS))
		return (NULL);

	arena->extent_hooks = (extent_hooks_t *)&extent_hooks_default;

	ql_new(&arena->extent_cache);
	if (malloc_mutex_init(&arena->extent_cache_mtx, "arena_extent_cache",
	    WITNESS_RANK_ARENA_EXTENT_CACHE))
		return (NULL);

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock, "arena_bin",
		    WITNESS_RANK_ARENA_BIN))
			return (NULL);
		bin->slabcur = NULL;
		extent_heap_new(&bin->slabs_nonfull);
		extent_init(&bin->slabs_full, arena, NULL, 0, 0, false, false,
		    false, false);
		if (config_stats)
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
	}

	return (arena);
}

void
arena_boot(void)
{

	arena_decay_time_default_set(opt_decay_time);
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_prefork(tsdn, &arena->lock);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_prefork(tsdn, &arena->extents_mtx);
}

void
arena_prefork2(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_prefork(tsdn, &arena->extent_cache_mtx);
}

void
arena_prefork3(tsdn_t *tsdn, arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_prefork(tsdn, &arena->bins[i].lock);
	malloc_mutex_prefork(tsdn, &arena->large_mtx);
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena)
{
	unsigned i;

	malloc_mutex_postfork_parent(tsdn, &arena->large_mtx);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_parent(tsdn, &arena->bins[i].lock);
	malloc_mutex_postfork_parent(tsdn, &arena->extent_cache_mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->extents_mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->lock);
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena)
{
	unsigned i;

	malloc_mutex_postfork_child(tsdn, &arena->large_mtx);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_child(tsdn, &arena->bins[i].lock);
	malloc_mutex_postfork_child(tsdn, &arena->extent_cache_mtx);
	malloc_mutex_postfork_child(tsdn, &arena->extents_mtx);
	malloc_mutex_postfork_child(tsdn, &arena->lock);
}
