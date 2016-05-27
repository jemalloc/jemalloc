#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

purge_mode_t	opt_purge = PURGE_DEFAULT;
const char	*purge_mode_names[] = {
	"ratio",
	"decay",
	"N/A"
};
ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
static ssize_t	lg_dirty_mult_default;
ssize_t		opt_decay_time = DECAY_TIME_DEFAULT;
static ssize_t	decay_time_default;

const arena_bin_info_t	arena_bin_info[NBINS] = {
#define	BIN_INFO_bin_yes(reg_size, run_size, nregs)			\
	{reg_size, run_size, nregs, BITMAP_INFO_INITIALIZER(nregs)},
#define	BIN_INFO_bin_no(reg_size, run_size, nregs)
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

size_t		map_bias;
size_t		map_misc_offset;
size_t		arena_maxrun; /* Max run size for arenas. */
size_t		large_maxclass; /* Max large size class. */
unsigned	nlclasses; /* Number of large size classes. */
unsigned	nhclasses; /* Number of huge size classes. */

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	arena_purge_to_limit(tsdn_t *tsdn, arena_t *arena,
    size_t ndirty_limit);
static void	arena_run_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, bool dirty, bool cleaned, bool decommitted);
static void	arena_dalloc_bin_run(tsdn_t *tsdn, arena_t *arena,
    arena_chunk_t *chunk, extent_t *extent, arena_run_t *run, arena_bin_t *bin);
static void	arena_bin_lower_run(tsdn_t *tsdn, arena_t *arena,
    extent_t *extent, arena_run_t *run, arena_bin_t *bin);

/******************************************************************************/

JEMALLOC_INLINE_C size_t
arena_miscelm_size_get(extent_t *extent, const arena_chunk_map_misc_t *miscelm)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	size_t pageind = arena_miscelm_to_pageind(extent, miscelm);
	size_t mapbits = arena_mapbits_get(chunk, pageind);
	return (arena_mapbits_size_decode(mapbits));
}

JEMALLOC_INLINE_C int
arena_run_addr_comp(const arena_chunk_map_misc_t *a,
    const arena_chunk_map_misc_t *b)
{
	uintptr_t a_miscelm = (uintptr_t)a;
	uintptr_t b_miscelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_miscelm > b_miscelm) - (a_miscelm < b_miscelm));
}

/* Generate pairing heap functions. */
ph_gen(static UNUSED, arena_run_heap_, arena_run_heap_t, arena_chunk_map_misc_t,
    ph_link, arena_run_addr_comp)

#ifdef JEMALLOC_JET
#undef run_quantize_floor
#define	run_quantize_floor JEMALLOC_N(n_run_quantize_floor)
#endif
static size_t
run_quantize_floor(size_t size)
{
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert(size <= HUGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	assert(size != 0);
	assert(size == PAGE_CEILING(size));

	pind = psz2ind(size - large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return (size);
	}
	ret = pind2sz(pind - 1) + large_pad;
	assert(ret <= size);
	return (ret);
}
#ifdef JEMALLOC_JET
#undef run_quantize_floor
#define	run_quantize_floor JEMALLOC_N(run_quantize_floor)
run_quantize_t *run_quantize_floor = JEMALLOC_N(n_run_quantize_floor);
#endif

#ifdef JEMALLOC_JET
#undef run_quantize_ceil
#define	run_quantize_ceil JEMALLOC_N(n_run_quantize_ceil)
#endif
static size_t
run_quantize_ceil(size_t size)
{
	size_t ret;

	assert(size > 0);
	assert(size <= HUGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = run_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large run,
		 * because under-sized runs may be mixed in.  This only happens
		 * when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = pind2sz(psz2ind(ret - large_pad + 1)) + large_pad;
	}
	return (ret);
}
#ifdef JEMALLOC_JET
#undef run_quantize_ceil
#define	run_quantize_ceil JEMALLOC_N(run_quantize_ceil)
run_quantize_t *run_quantize_ceil = JEMALLOC_N(n_run_quantize_ceil);
#endif

static void
arena_avail_insert(arena_t *arena, extent_t *extent, size_t pageind,
    size_t npages)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	pszind_t pind = psz2ind(run_quantize_floor(arena_miscelm_size_get(
	    extent, arena_miscelm_get_const(chunk, pageind))));
	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_run_heap_insert(&arena->runs_avail[pind],
	    arena_miscelm_get_mutable(chunk, pageind));
}

static void
arena_avail_remove(arena_t *arena, extent_t *extent, size_t pageind,
    size_t npages)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	pszind_t pind = psz2ind(run_quantize_floor(arena_miscelm_size_get(
	    extent, arena_miscelm_get_const(chunk, pageind))));
	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_run_heap_remove(&arena->runs_avail[pind],
	    arena_miscelm_get_mutable(chunk, pageind));
}

static void
arena_run_dirty_insert(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get_mutable(chunk,
	    pageind);

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);

	qr_new(&miscelm->rd, rd_link);
	qr_meld(&arena->runs_dirty, &miscelm->rd, rd_link);
	arena->ndirty += npages;
}

static void
arena_run_dirty_remove(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get_mutable(chunk,
	    pageind);

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);

	qr_remove(&miscelm->rd, rd_link);
	assert(arena->ndirty >= npages);
	arena->ndirty -= npages;
}

static size_t
arena_chunk_dirty_npages(const extent_t *extent)
{

	return (extent_size_get(extent) >> LG_PAGE);
}

static extent_t *
arena_chunk_cache_alloc_locked(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *new_addr, size_t size, size_t alignment,
    bool *zero, bool slab)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	return (chunk_alloc_cache(tsdn, arena, chunk_hooks, new_addr, size,
	    alignment, zero, slab));
}

extent_t *
arena_chunk_cache_alloc(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, void *new_addr, size_t size, size_t alignment,
    bool *zero)
{
	extent_t *extent;

	malloc_mutex_lock(tsdn, &arena->lock);
	extent = arena_chunk_cache_alloc_locked(tsdn, arena, chunk_hooks,
	    new_addr, size, alignment, zero, false);
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (extent);
}

static void
arena_chunk_cache_dalloc_locked(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, extent_t *extent)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	chunk_dalloc_cache(tsdn, arena, chunk_hooks, extent);
	arena_maybe_purge(tsdn, arena);
}

void
arena_chunk_cache_dalloc(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, extent_t *extent)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_chunk_cache_dalloc_locked(tsdn, arena, chunk_hooks, extent);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_chunk_cache_maybe_insert(arena_t *arena, extent_t *extent, bool cache)
{

	if (cache) {
		extent_dirty_insert(extent, &arena->runs_dirty,
		    &arena->chunks_cache);
		arena->ndirty += arena_chunk_dirty_npages(extent);
	}
}

void
arena_chunk_cache_maybe_remove(arena_t *arena, extent_t *extent, bool dirty)
{

	if (dirty) {
		extent_dirty_remove(extent);
		assert(arena->ndirty >= arena_chunk_dirty_npages(extent));
		arena->ndirty -= arena_chunk_dirty_npages(extent);
	}
}

JEMALLOC_INLINE_C void *
arena_run_reg_alloc(tsdn_t *tsdn, arena_run_t *run,
    const arena_bin_info_t *bin_info)
{
	void *ret;
	extent_t *extent;
	size_t regind;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(run->nfree > 0);
	assert(!bitmap_full(run->bitmap, &bin_info->bitmap_info));

	extent = iealloc(tsdn, run);
	regind = (unsigned)bitmap_sfu(run->bitmap, &bin_info->bitmap_info);
	miscelm = arena_run_to_miscelm(extent, run);
	rpages = arena_miscelm_to_rpages(extent, miscelm);
	ret = (void *)((uintptr_t)rpages + (uintptr_t)(bin_info->reg_size *
	    regind));
	run->nfree--;
	return (ret);
}

JEMALLOC_INLINE_C size_t
arena_run_regind(extent_t *extent, arena_run_t *run,
    const arena_bin_info_t *bin_info, const void *ptr)
{
	size_t diff, interval, shift, regind;
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(extent, run);
	void *rpages = arena_miscelm_to_rpages(extent, miscelm);

	/*
	 * Freeing a pointer lower than region zero can cause assertion
	 * failure.
	 */
	assert((uintptr_t)ptr >= (uintptr_t)rpages);

	/*
	 * Avoid doing division with a variable divisor if possible.  Using
	 * actual division here can reduce allocator throughput by over 20%!
	 */
	diff = (size_t)((uintptr_t)ptr - (uintptr_t)rpages);

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
#define	SIZE_INV_SHIFT	((sizeof(size_t) << 3) - LG_RUN_MAXREGS)
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
arena_run_reg_dalloc(tsdn_t *tsdn, arena_run_t *run, extent_t *extent,
    void *ptr)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t mapbits = arena_mapbits_get(chunk, pageind);
	szind_t binind = arena_ptr_small_binind_get(tsdn, ptr, mapbits);
	const arena_bin_info_t *bin_info = &arena_bin_info[binind];
	size_t regind = arena_run_regind(extent, run, bin_info, ptr);

	assert(run->nfree < bin_info->nregs);
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr -
	    (uintptr_t)arena_miscelm_to_rpages(extent,
	    arena_run_to_miscelm(extent, run))) % (uintptr_t)bin_info->reg_size
	    == 0);
	assert((uintptr_t)ptr >=
	    (uintptr_t)arena_miscelm_to_rpages(extent,
	    arena_run_to_miscelm(extent, run)));
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(run->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(run->bitmap, &bin_info->bitmap_info, regind);
	run->nfree++;
}

JEMALLOC_INLINE_C void
arena_run_zero(arena_chunk_t *chunk, size_t run_ind, size_t npages)
{

	memset((void *)((uintptr_t)chunk + (run_ind << LG_PAGE)), 0,
	    (npages << LG_PAGE));
}

JEMALLOC_INLINE_C void
arena_run_page_validate_zeroed(arena_chunk_t *chunk, size_t run_ind)
{
	size_t i;
	UNUSED size_t *p = (size_t *)((uintptr_t)chunk + (run_ind << LG_PAGE));

	for (i = 0; i < PAGE / sizeof(size_t); i++)
		assert(p[i] == 0);
}

static void
arena_nactive_add(arena_t *arena, size_t add_pages)
{

	if (config_stats) {
		size_t cactive_add = CHUNK_CEILING((arena->nactive +
		    add_pages) << LG_PAGE) - CHUNK_CEILING(arena->nactive <<
		    LG_PAGE);
		if (cactive_add != 0)
			stats_cactive_add(cactive_add);
	}
	arena->nactive += add_pages;
}

static void
arena_nactive_sub(arena_t *arena, size_t sub_pages)
{

	if (config_stats) {
		size_t cactive_sub = CHUNK_CEILING(arena->nactive << LG_PAGE) -
		    CHUNK_CEILING((arena->nactive - sub_pages) << LG_PAGE);
		if (cactive_sub != 0)
			stats_cactive_sub(cactive_sub);
	}
	arena->nactive -= sub_pages;
}

static void
arena_run_split_remove(arena_t *arena, extent_t *extent, size_t run_ind,
    size_t flag_dirty, size_t flag_decommitted, size_t need_pages)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	size_t total_pages, rem_pages;

	assert(flag_dirty == 0 || flag_decommitted == 0);

	total_pages = arena_mapbits_unallocated_size_get(chunk, run_ind) >>
	    LG_PAGE;
	assert(arena_mapbits_dirty_get(chunk, run_ind+total_pages-1) ==
	    flag_dirty);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_remove(arena, extent, run_ind, total_pages);
	if (flag_dirty != 0)
		arena_run_dirty_remove(arena, chunk, run_ind, total_pages);
	arena_nactive_add(arena, need_pages);

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		size_t flags = flag_dirty | flag_decommitted;
		size_t flag_unzeroed_mask = (flags == 0) ?  CHUNK_MAP_UNZEROED :
		    0;

		arena_mapbits_unallocated_set(chunk, run_ind+need_pages,
		    (rem_pages << LG_PAGE), flags |
		    (arena_mapbits_unzeroed_get(chunk, run_ind+need_pages) &
		    flag_unzeroed_mask));
		arena_mapbits_unallocated_set(chunk, run_ind+total_pages-1,
		    (rem_pages << LG_PAGE), flags |
		    (arena_mapbits_unzeroed_get(chunk, run_ind+total_pages-1) &
		    flag_unzeroed_mask));
		if (flag_dirty != 0) {
			arena_run_dirty_insert(arena, chunk, run_ind+need_pages,
			    rem_pages);
		}
		arena_avail_insert(arena, extent, run_ind+need_pages,
		    rem_pages);
	}
}

static bool
arena_run_split_large_helper(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, size_t size, bool remove, bool zero)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, flag_decommitted, run_ind, need_pages;
	size_t flag_unzeroed_mask;

	chunk = (arena_chunk_t *)extent_addr_get(extent);
	miscelm = arena_run_to_miscelm(extent, run);
	run_ind = arena_miscelm_to_pageind(extent, miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	flag_decommitted = arena_mapbits_decommitted_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	if (flag_decommitted != 0 && chunk_commit_wrapper(tsdn, arena,
	    &arena->chunk_hooks, extent, run_ind << LG_PAGE, size))
		return (true);

	if (remove) {
		arena_run_split_remove(arena, extent, run_ind, flag_dirty,
		    flag_decommitted, need_pages);
	}

	if (zero) {
		if (flag_decommitted != 0)
			; /* The run is untouched, and therefore zeroed. */
		else if (flag_dirty != 0) {
			/* The run is dirty, so all pages must be zeroed. */
			arena_run_zero(chunk, run_ind, need_pages);
		} else {
			/*
			 * The run is clean, so some pages may be zeroed (i.e.
			 * never before touched).
			 */
			size_t i;
			for (i = 0; i < need_pages; i++) {
				if (arena_mapbits_unzeroed_get(chunk, run_ind+i)
				    != 0)
					arena_run_zero(chunk, run_ind+i, 1);
				else if (config_debug) {
					arena_run_page_validate_zeroed(chunk,
					    run_ind+i);
				}
			}
		}
	}

	/*
	 * Set the last element first, in case the run only contains one page
	 * (i.e. both statements set the same element).
	 */
	flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;
	arena_mapbits_large_set(chunk, run_ind+need_pages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    run_ind+need_pages-1)));
	arena_mapbits_large_set(chunk, run_ind, size, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, run_ind)));
	return (false);
}

static bool
arena_run_split_large(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, size_t size, bool zero)
{

	return (arena_run_split_large_helper(tsdn, arena, extent, run, size,
	    true, zero));
}

static bool
arena_run_init_large(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, size_t size, bool zero)
{

	return (arena_run_split_large_helper(tsdn, arena, extent, run, size,
	    false, zero));
}

static bool
arena_run_split_small(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, size_t size, szind_t binind)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, flag_decommitted, run_ind, need_pages, i;

	assert(binind != BININD_INVALID);

	chunk = (arena_chunk_t *)extent_addr_get(extent);
	miscelm = arena_run_to_miscelm(extent, run);
	run_ind = arena_miscelm_to_pageind(extent, miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	flag_decommitted = arena_mapbits_decommitted_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	if (flag_decommitted != 0 && chunk_commit_wrapper(tsdn, arena,
	    &arena->chunk_hooks, extent, run_ind << LG_PAGE, size))
		return (true);

	arena_run_split_remove(arena, extent, run_ind, flag_dirty,
	    flag_decommitted, need_pages);

	for (i = 0; i < need_pages; i++) {
		size_t flag_unzeroed = arena_mapbits_unzeroed_get(chunk,
		    run_ind+i);
		arena_mapbits_small_set(chunk, run_ind+i, i, binind,
		    flag_unzeroed);
		if (config_debug && flag_dirty == 0 && flag_unzeroed == 0)
			arena_run_page_validate_zeroed(chunk, run_ind+i);
	}
	return (false);
}

static extent_t *
arena_chunk_init_spare(arena_t *arena)
{
	extent_t *extent;

	assert(arena->spare != NULL);

	extent = arena->spare;
	arena->spare = NULL;

	assert(arena_mapbits_allocated_get((arena_chunk_t *)
	    extent_addr_get(extent), map_bias) == 0);
	assert(arena_mapbits_allocated_get((arena_chunk_t *)
	    extent_addr_get(extent), chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get((arena_chunk_t *)
	    extent_addr_get(extent), map_bias) == arena_maxrun);
	assert(arena_mapbits_unallocated_size_get((arena_chunk_t *)
	    extent_addr_get(extent), chunk_npages-1) == arena_maxrun);
	assert(arena_mapbits_dirty_get((arena_chunk_t *)
	    extent_addr_get(extent), map_bias) ==
	    arena_mapbits_dirty_get((arena_chunk_t *)extent_addr_get(extent),
	    chunk_npages-1));

	return (extent);
}

static extent_t *
arena_chunk_alloc_internal_hard(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, bool *zero, bool *commit)
{
	extent_t *extent;

	malloc_mutex_unlock(tsdn, &arena->lock);

	extent = chunk_alloc_wrapper(tsdn, arena, chunk_hooks, NULL, chunksize,
	    PAGE, zero, commit, true);
	if (extent != NULL && !*commit) {
		/* Commit header. */
		if (chunk_commit_wrapper(tsdn, arena, chunk_hooks, extent, 0,
		    map_bias << LG_PAGE)) {
			chunk_dalloc_wrapper(tsdn, arena, chunk_hooks, extent);
			extent = NULL;
		}
	}

	malloc_mutex_lock(tsdn, &arena->lock);

	return (extent);
}

static extent_t *
arena_chunk_alloc_internal(tsdn_t *tsdn, arena_t *arena, bool *zero,
    bool *commit)
{
	extent_t *extent;
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;

	extent = arena_chunk_cache_alloc_locked(tsdn, arena, &chunk_hooks, NULL,
	    chunksize, PAGE, zero, true);
	if (extent != NULL)
		*commit = true;
	if (extent == NULL) {
		extent = arena_chunk_alloc_internal_hard(tsdn, arena,
		    &chunk_hooks, zero, commit);
		if (extent == NULL)
			return (NULL);
	}
	assert(extent_slab_get(extent));

	if (config_stats) {
		arena->stats.mapped += extent_size_get(extent);
		arena->stats.metadata_mapped += (map_bias << LG_PAGE);
	}

	return (extent);
}

static extent_t *
arena_chunk_init_hard(tsdn_t *tsdn, arena_t *arena)
{
	extent_t *extent;
	bool zero, commit;
	size_t flag_unzeroed, flag_decommitted, i;

	assert(arena->spare == NULL);

	zero = false;
	commit = false;
	extent = arena_chunk_alloc_internal(tsdn, arena, &zero, &commit);
	if (extent == NULL)
		return (NULL);

	/*
	 * Initialize the map to contain one maximal free untouched run.  Mark
	 * the pages as zeroed if arena_chunk_alloc_internal() returned a zeroed
	 * or decommitted chunk.
	 */
	flag_unzeroed = (zero || !commit) ? 0 : CHUNK_MAP_UNZEROED;
	flag_decommitted = commit ? 0 : CHUNK_MAP_DECOMMITTED;
	arena_mapbits_unallocated_set((arena_chunk_t *)extent_addr_get(extent),
	    map_bias, arena_maxrun, flag_unzeroed | flag_decommitted);
	/*
	 * There is no need to initialize the internal page map entries unless
	 * the chunk is not zeroed.
	 */
	if (!zero) {
		for (i = map_bias+1; i < chunk_npages-1; i++) {
			arena_mapbits_internal_set((arena_chunk_t *)
			    extent_addr_get(extent), i, flag_unzeroed);
		}
	} else {
		if (config_debug) {
			for (i = map_bias+1; i < chunk_npages-1; i++) {
				assert(arena_mapbits_unzeroed_get(
				    (arena_chunk_t *)extent_addr_get(extent), i)
				    == flag_unzeroed);
			}
		}
	}
	arena_mapbits_unallocated_set((arena_chunk_t *)extent_addr_get(extent),
	    chunk_npages-1, arena_maxrun, flag_unzeroed);

	return (extent);
}

static extent_t *
arena_chunk_alloc(tsdn_t *tsdn, arena_t *arena)
{
	extent_t *extent;

	if (arena->spare != NULL)
		extent = arena_chunk_init_spare(arena);
	else {
		extent = arena_chunk_init_hard(tsdn, arena);
		if (extent == NULL)
			return (NULL);
	}

	ql_elm_new(extent, ql_link);
	ql_tail_insert(&arena->achunks, extent, ql_link);
	arena_avail_insert(arena, extent, map_bias, chunk_npages-map_bias);

	return (extent);
}

static void
arena_chunk_discard(tsdn_t *tsdn, arena_t *arena, extent_t *extent)
{
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;

	extent_committed_set(extent,
	    (arena_mapbits_decommitted_get((arena_chunk_t *)
	    extent_addr_get(extent), map_bias) == 0));
	if (!extent_committed_get(extent)) {
		/*
		 * Decommit the header.  Mark the chunk as decommitted even if
		 * header decommit fails, since treating a partially committed
		 * chunk as committed has a high potential for causing later
		 * access of decommitted memory.
		 */
		chunk_decommit_wrapper(tsdn, arena, &chunk_hooks, extent, 0,
		    map_bias << LG_PAGE);
	}

	if (config_stats) {
		arena->stats.mapped -= extent_size_get(extent);
		arena->stats.metadata_mapped -= (map_bias << LG_PAGE);
	}

	arena_chunk_cache_dalloc_locked(tsdn, arena, &chunk_hooks, extent);
}

static void
arena_spare_discard(tsdn_t *tsdn, arena_t *arena, extent_t *spare)
{

	assert(arena->spare != spare);

	if (arena_mapbits_dirty_get((arena_chunk_t *)extent_addr_get(spare),
	    map_bias) != 0) {
		arena_run_dirty_remove(arena, (arena_chunk_t *)
		    extent_addr_get(spare), map_bias, chunk_npages-map_bias);
	}

	arena_chunk_discard(tsdn, arena, spare);
}

static void
arena_chunk_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	extent_t *spare;

	assert(arena_mapbits_allocated_get(chunk, map_bias) == 0);
	assert(arena_mapbits_allocated_get(chunk, chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get(chunk, map_bias) ==
	    arena_maxrun);
	assert(arena_mapbits_unallocated_size_get(chunk, chunk_npages-1) ==
	    arena_maxrun);
	assert(arena_mapbits_dirty_get(chunk, map_bias) ==
	    arena_mapbits_dirty_get(chunk, chunk_npages-1));
	assert(arena_mapbits_decommitted_get(chunk, map_bias) ==
	    arena_mapbits_decommitted_get(chunk, chunk_npages-1));

	/* Remove run from runs_avail, so that the arena does not use it. */
	arena_avail_remove(arena, extent, map_bias, chunk_npages-map_bias);

	ql_remove(&arena->achunks, extent, ql_link);
	spare = arena->spare;
	arena->spare = extent;
	if (spare != NULL)
		arena_spare_discard(tsdn, arena, spare);
}

static void
arena_huge_malloc_stats_update(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.nmalloc_huge++;
	arena->stats.allocated_huge += usize;
	arena->stats.hstats[index].nmalloc++;
	arena->stats.hstats[index].curhchunks++;
}

static void
arena_huge_malloc_stats_update_undo(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.nmalloc_huge--;
	arena->stats.allocated_huge -= usize;
	arena->stats.hstats[index].nmalloc--;
	arena->stats.hstats[index].curhchunks--;
}

static void
arena_huge_dalloc_stats_update(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.ndalloc_huge++;
	arena->stats.allocated_huge -= usize;
	arena->stats.hstats[index].ndalloc++;
	arena->stats.hstats[index].curhchunks--;
}

static void
arena_huge_reset_stats_cancel(arena_t *arena, size_t usize)
{
	szind_t index = size2index(usize) - nlclasses - NBINS;

	cassert(config_stats);

	arena->stats.ndalloc_huge++;
	arena->stats.hstats[index].ndalloc--;
}

static void
arena_huge_ralloc_stats_update(arena_t *arena, size_t oldsize, size_t usize)
{

	arena_huge_dalloc_stats_update(arena, oldsize);
	arena_huge_malloc_stats_update(arena, usize);
}

static extent_t *
arena_chunk_alloc_huge_hard(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, size_t usize, size_t alignment, bool *zero)
{
	extent_t *extent;
	bool commit = true;

	extent = chunk_alloc_wrapper(tsdn, arena, chunk_hooks, NULL, usize,
	    alignment, zero, &commit, false);
	if (extent == NULL) {
		/* Revert optimistic stats updates. */
		malloc_mutex_lock(tsdn, &arena->lock);
		if (config_stats) {
			arena_huge_malloc_stats_update_undo(arena, usize);
			arena->stats.mapped -= usize;
		}
		arena_nactive_sub(arena, usize >> LG_PAGE);
		malloc_mutex_unlock(tsdn, &arena->lock);
	}

	return (extent);
}

extent_t *
arena_chunk_alloc_huge(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool *zero)
{
	extent_t *extent;
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;

	malloc_mutex_lock(tsdn, &arena->lock);

	/* Optimistically update stats. */
	if (config_stats) {
		arena_huge_malloc_stats_update(arena, usize);
		arena->stats.mapped += usize;
	}
	arena_nactive_add(arena, usize >> LG_PAGE);

	extent = arena_chunk_cache_alloc_locked(tsdn, arena, &chunk_hooks, NULL,
	    usize, alignment, zero, false);
	malloc_mutex_unlock(tsdn, &arena->lock);
	if (extent == NULL) {
		extent = arena_chunk_alloc_huge_hard(tsdn, arena, &chunk_hooks,
		    usize, alignment, zero);
	}

	return (extent);
}

void
arena_chunk_dalloc_huge(tsdn_t *tsdn, arena_t *arena, extent_t *extent)
{
	chunk_hooks_t chunk_hooks = CHUNK_HOOKS_INITIALIZER;

	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_stats) {
		arena_huge_dalloc_stats_update(arena, extent_size_get(extent));
		arena->stats.mapped -= extent_size_get(extent);
	}
	arena_nactive_sub(arena, extent_size_get(extent) >> LG_PAGE);

	arena_chunk_cache_dalloc_locked(tsdn, arena, &chunk_hooks, extent);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_chunk_ralloc_huge_shrink(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldsize)
{
	size_t usize = extent_size_get(extent);
	size_t udiff = oldsize - usize;
	size_t cdiff = CHUNK_CEILING(oldsize) - CHUNK_CEILING(usize);

	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_stats) {
		arena_huge_ralloc_stats_update(arena, oldsize, usize);
		if (cdiff != 0)
			arena->stats.mapped -= cdiff;
	}
	arena_nactive_sub(arena, udiff >> LG_PAGE);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_chunk_ralloc_huge_expand(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    size_t oldsize)
{
	size_t usize = extent_size_get(extent);
	size_t cdiff = CHUNK_CEILING(usize) - CHUNK_CEILING(oldsize);
	size_t udiff = usize - oldsize;

	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_stats) {
		arena_huge_ralloc_stats_update(arena, oldsize, usize);
		arena->stats.mapped += cdiff;
	}
	arena_nactive_add(arena, udiff >> LG_PAGE);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

/*
 * Do first-best-fit run selection, i.e. select the lowest run that best fits.
 * Run sizes are indexed, so not all candidate runs are necessarily exactly the
 * same size.
 */
static arena_run_t *
arena_run_first_best_fit(arena_t *arena, size_t size)
{
	pszind_t pind, i;

	pind = psz2ind(run_quantize_ceil(size));

	for (i = pind; pind2sz(i) <= large_maxclass; i++) {
		arena_chunk_map_misc_t *miscelm = arena_run_heap_first(
		    &arena->runs_avail[i]);
		if (miscelm != NULL)
			return (&miscelm->run);
	}

	return (NULL);
}

static arena_run_t *
arena_run_alloc_large_helper(tsdn_t *tsdn, arena_t *arena, size_t size,
    bool zero)
{
	arena_run_t *run = arena_run_first_best_fit(arena, s2u(size));
	if (run != NULL) {
		if (arena_run_split_large(tsdn, arena, iealloc(tsdn, run), run,
		    size, zero))
			run = NULL;
	}
	return (run);
}

static arena_run_t *
arena_run_alloc_large(tsdn_t *tsdn, arena_t *arena, size_t size, bool zero)
{
	arena_run_t *run;
	extent_t *extent;

	assert(size <= arena_maxrun);
	assert(size == PAGE_CEILING(size));

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_large_helper(tsdn, arena, size, zero);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	extent = arena_chunk_alloc(tsdn, arena);
	if (extent != NULL) {
		run = &arena_miscelm_get_mutable((arena_chunk_t *)
		    extent_addr_get(extent), map_bias)->run;
		if (arena_run_split_large(tsdn, arena, iealloc(tsdn, run), run,
		    size, zero))
			run = NULL;
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_large_helper(tsdn, arena, size, zero));
}

static arena_run_t *
arena_run_alloc_small_helper(tsdn_t *tsdn, arena_t *arena, size_t size,
    szind_t binind)
{
	arena_run_t *run = arena_run_first_best_fit(arena, size);
	if (run != NULL) {
		if (arena_run_split_small(tsdn, arena, iealloc(tsdn, run), run,
		    size, binind))
			run = NULL;
	}
	return (run);
}

static arena_run_t *
arena_run_alloc_small(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t binind)
{
	arena_run_t *run;
	extent_t *extent;

	assert(size <= arena_maxrun);
	assert(size == PAGE_CEILING(size));
	assert(binind != BININD_INVALID);

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_small_helper(tsdn, arena, size, binind);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	extent = arena_chunk_alloc(tsdn, arena);
	if (extent != NULL) {
		run = &arena_miscelm_get_mutable(
		    (arena_chunk_t *)extent_addr_get(extent), map_bias)->run;
		if (arena_run_split_small(tsdn, arena, iealloc(tsdn, run), run,
		    size, binind))
			run = NULL;
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_small_helper(tsdn, arena, size, binind));
}

static bool
arena_lg_dirty_mult_valid(ssize_t lg_dirty_mult)
{

	return (lg_dirty_mult >= -1 && lg_dirty_mult < (ssize_t)(sizeof(size_t)
	    << 3));
}

ssize_t
arena_lg_dirty_mult_get(tsdn_t *tsdn, arena_t *arena)
{
	ssize_t lg_dirty_mult;

	malloc_mutex_lock(tsdn, &arena->lock);
	lg_dirty_mult = arena->lg_dirty_mult;
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (lg_dirty_mult);
}

bool
arena_lg_dirty_mult_set(tsdn_t *tsdn, arena_t *arena, ssize_t lg_dirty_mult)
{

	if (!arena_lg_dirty_mult_valid(lg_dirty_mult))
		return (true);

	malloc_mutex_lock(tsdn, &arena->lock);
	arena->lg_dirty_mult = lg_dirty_mult;
	arena_maybe_purge(tsdn, arena);
	malloc_mutex_unlock(tsdn, &arena->lock);

	return (false);
}

static void
arena_decay_deadline_init(arena_t *arena)
{

	assert(opt_purge == purge_mode_decay);

	/*
	 * Generate a new deadline that is uniformly random within the next
	 * epoch after the current one.
	 */
	nstime_copy(&arena->decay_deadline, &arena->decay_epoch);
	nstime_add(&arena->decay_deadline, &arena->decay_interval);
	if (arena->decay_time > 0) {
		nstime_t jitter;

		nstime_init(&jitter, prng_range(&arena->decay_jitter_state,
		    nstime_ns(&arena->decay_interval)));
		nstime_add(&arena->decay_deadline, &jitter);
	}
}

static bool
arena_decay_deadline_reached(const arena_t *arena, const nstime_t *time)
{

	assert(opt_purge == purge_mode_decay);

	return (nstime_compare(&arena->decay_deadline, time) <= 0);
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

	assert(opt_purge == purge_mode_decay);

	/*
	 * For each element of decay_backlog, multiply by the corresponding
	 * fixed-point smoothstep decay factor.  Sum the products, then divide
	 * to round down to the nearest whole number of pages.
	 */
	sum = 0;
	for (i = 0; i < SMOOTHSTEP_NSTEPS; i++)
		sum += arena->decay_backlog[i] * h_steps[i];
	npages_limit_backlog = (size_t)(sum >> SMOOTHSTEP_BFP);

	return (npages_limit_backlog);
}

static void
arena_decay_epoch_advance(arena_t *arena, const nstime_t *time)
{
	uint64_t nadvance_u64;
	nstime_t delta;
	size_t ndirty_delta;

	assert(opt_purge == purge_mode_decay);
	assert(arena_decay_deadline_reached(arena, time));

	nstime_copy(&delta, time);
	nstime_subtract(&delta, &arena->decay_epoch);
	nadvance_u64 = nstime_divide(&delta, &arena->decay_interval);
	assert(nadvance_u64 > 0);

	/* Add nadvance_u64 decay intervals to epoch. */
	nstime_copy(&delta, &arena->decay_interval);
	nstime_imultiply(&delta, nadvance_u64);
	nstime_add(&arena->decay_epoch, &delta);

	/* Set a new deadline. */
	arena_decay_deadline_init(arena);

	/* Update the backlog. */
	if (nadvance_u64 >= SMOOTHSTEP_NSTEPS) {
		memset(arena->decay_backlog, 0, (SMOOTHSTEP_NSTEPS-1) *
		    sizeof(size_t));
	} else {
		size_t nadvance_z = (size_t)nadvance_u64;

		assert((uint64_t)nadvance_z == nadvance_u64);

		memmove(arena->decay_backlog, &arena->decay_backlog[nadvance_z],
		    (SMOOTHSTEP_NSTEPS - nadvance_z) * sizeof(size_t));
		if (nadvance_z > 1) {
			memset(&arena->decay_backlog[SMOOTHSTEP_NSTEPS -
			    nadvance_z], 0, (nadvance_z-1) * sizeof(size_t));
		}
	}
	ndirty_delta = (arena->ndirty > arena->decay_ndirty) ? arena->ndirty -
	    arena->decay_ndirty : 0;
	arena->decay_ndirty = arena->ndirty;
	arena->decay_backlog[SMOOTHSTEP_NSTEPS-1] = ndirty_delta;
	arena->decay_backlog_npages_limit =
	    arena_decay_backlog_npages_limit(arena);
}

static size_t
arena_decay_npages_limit(arena_t *arena)
{
	size_t npages_limit;

	assert(opt_purge == purge_mode_decay);

	npages_limit = arena->decay_backlog_npages_limit;

	/* Add in any dirty pages created during the current epoch. */
	if (arena->ndirty > arena->decay_ndirty)
		npages_limit += arena->ndirty - arena->decay_ndirty;

	return (npages_limit);
}

static void
arena_decay_init(arena_t *arena, ssize_t decay_time)
{

	arena->decay_time = decay_time;
	if (decay_time > 0) {
		nstime_init2(&arena->decay_interval, decay_time, 0);
		nstime_idivide(&arena->decay_interval, SMOOTHSTEP_NSTEPS);
	}

	nstime_init(&arena->decay_epoch, 0);
	nstime_update(&arena->decay_epoch);
	arena->decay_jitter_state = (uint64_t)(uintptr_t)arena;
	arena_decay_deadline_init(arena);
	arena->decay_ndirty = arena->ndirty;
	arena->decay_backlog_npages_limit = 0;
	memset(arena->decay_backlog, 0, SMOOTHSTEP_NSTEPS * sizeof(size_t));
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
	decay_time = arena->decay_time;
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
arena_maybe_purge_ratio(tsdn_t *tsdn, arena_t *arena)
{

	assert(opt_purge == purge_mode_ratio);

	/* Don't purge if the option is disabled. */
	if (arena->lg_dirty_mult < 0)
		return;

	/*
	 * Iterate, since preventing recursive purging could otherwise leave too
	 * many dirty pages.
	 */
	while (true) {
		size_t threshold = (arena->nactive >> arena->lg_dirty_mult);
		if (threshold < chunk_npages)
			threshold = chunk_npages;
		/*
		 * Don't purge unless the number of purgeable pages exceeds the
		 * threshold.
		 */
		if (arena->ndirty <= threshold)
			return;
		arena_purge_to_limit(tsdn, arena, threshold);
	}
}

static void
arena_maybe_purge_decay(tsdn_t *tsdn, arena_t *arena)
{
	nstime_t time;
	size_t ndirty_limit;

	assert(opt_purge == purge_mode_decay);

	/* Purge all or nothing if the option is disabled. */
	if (arena->decay_time <= 0) {
		if (arena->decay_time == 0)
			arena_purge_to_limit(tsdn, arena, 0);
		return;
	}

	nstime_copy(&time, &arena->decay_epoch);
	if (unlikely(nstime_update(&time))) {
		/* Time went backwards.  Force an epoch advance. */
		nstime_copy(&time, &arena->decay_deadline);
	}

	if (arena_decay_deadline_reached(arena, &time))
		arena_decay_epoch_advance(arena, &time);

	ndirty_limit = arena_decay_npages_limit(arena);

	/*
	 * Don't try to purge unless the number of purgeable pages exceeds the
	 * current limit.
	 */
	if (arena->ndirty <= ndirty_limit)
		return;
	arena_purge_to_limit(tsdn, arena, ndirty_limit);
}

void
arena_maybe_purge(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_assert_owner(tsdn, &arena->lock);

	/* Don't recursively purge. */
	if (arena->purging)
		return;

	if (opt_purge == purge_mode_ratio)
		arena_maybe_purge_ratio(tsdn, arena);
	else
		arena_maybe_purge_decay(tsdn, arena);
}

static size_t
arena_dirty_count(tsdn_t *tsdn, arena_t *arena)
{
	size_t ndirty = 0;
	arena_runs_dirty_link_t *rdelm;
	extent_t *chunkselm;

	for (rdelm = qr_next(&arena->runs_dirty, rd_link),
	    chunkselm = qr_next(&arena->chunks_cache, cc_link);
	    rdelm != &arena->runs_dirty; rdelm = qr_next(rdelm, rd_link)) {
		size_t npages;

		if (rdelm == &chunkselm->rd) {
			npages = extent_size_get(chunkselm) >> LG_PAGE;
			chunkselm = qr_next(chunkselm, cc_link);
		} else {
			extent_t *extent = iealloc(tsdn, rdelm);
			arena_chunk_t *chunk =
			    (arena_chunk_t *)extent_addr_get(extent);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(extent, rdelm);
			size_t pageind = arena_miscelm_to_pageind(extent,
			    miscelm);
			assert(arena_mapbits_allocated_get(chunk, pageind) ==
			    0);
			assert(arena_mapbits_large_get(chunk, pageind) == 0);
			assert(arena_mapbits_dirty_get(chunk, pageind) != 0);
			npages = arena_mapbits_unallocated_size_get(chunk,
			    pageind) >> LG_PAGE;
		}
		ndirty += npages;
	}

	return (ndirty);
}

static size_t
arena_stash_dirty(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    size_t ndirty_limit, arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_t *purge_chunks_sentinel)
{
	arena_runs_dirty_link_t *rdelm, *rdelm_next;
	extent_t *chunkselm;
	size_t nstashed = 0;

	/* Stash runs/chunks according to ndirty_limit. */
	for (rdelm = qr_next(&arena->runs_dirty, rd_link),
	    chunkselm = qr_next(&arena->chunks_cache, cc_link);
	    rdelm != &arena->runs_dirty; rdelm = rdelm_next) {
		size_t npages;
		rdelm_next = qr_next(rdelm, rd_link);

		if (rdelm == &chunkselm->rd) {
			extent_t *chunkselm_next;
			bool zero;
			UNUSED extent_t *extent;

			npages = extent_size_get(chunkselm) >> LG_PAGE;
			if (opt_purge == purge_mode_decay && arena->ndirty -
			    (nstashed + npages) < ndirty_limit)
				break;

			chunkselm_next = qr_next(chunkselm, cc_link);
			/* Allocate. */
			zero = false;
			extent = arena_chunk_cache_alloc_locked(tsdn, arena,
			    chunk_hooks, extent_addr_get(chunkselm),
			    extent_size_get(chunkselm), PAGE, &zero, false);
			assert(extent == chunkselm);
			assert(zero == extent_zeroed_get(chunkselm));
			extent_dirty_insert(chunkselm, purge_runs_sentinel,
			    purge_chunks_sentinel);
			assert(npages == (extent_size_get(chunkselm) >>
			    LG_PAGE));
			chunkselm = chunkselm_next;
		} else {
			extent_t *extent = iealloc(tsdn, rdelm);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(extent, rdelm);
			size_t pageind = arena_miscelm_to_pageind(extent,
			    miscelm);
			arena_run_t *run = &miscelm->run;
			size_t run_size =
			    arena_mapbits_unallocated_size_get((arena_chunk_t *)
			    extent_addr_get(extent), pageind);

			npages = run_size >> LG_PAGE;
			if (opt_purge == purge_mode_decay && arena->ndirty -
			    (nstashed + npages) < ndirty_limit)
				break;

			assert(pageind + npages <= chunk_npages);
			assert(arena_mapbits_dirty_get((arena_chunk_t *)
			    extent_addr_get(extent), pageind) ==
			    arena_mapbits_dirty_get((arena_chunk_t *)
			    extent_addr_get(extent), pageind+npages-1));

			/*
			 * If purging the spare chunk's run, make it available
			 * prior to allocation.
			 */
			if (extent == arena->spare)
				arena_chunk_alloc(tsdn, arena);

			/* Temporarily allocate the free dirty run. */
			arena_run_split_large(tsdn, arena, extent, run,
			    run_size, false);
			/* Stash. */
			if (false)
				qr_new(rdelm, rd_link); /* Redundant. */
			else {
				assert(qr_next(rdelm, rd_link) == rdelm);
				assert(qr_prev(rdelm, rd_link) == rdelm);
			}
			qr_meld(purge_runs_sentinel, rdelm, rd_link);
		}

		nstashed += npages;
		if (opt_purge == purge_mode_ratio && arena->ndirty - nstashed <=
		    ndirty_limit)
			break;
	}

	return (nstashed);
}

static size_t
arena_purge_stashed(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_t *purge_chunks_sentinel)
{
	size_t npurged, nmadvise;
	arena_runs_dirty_link_t *rdelm;
	extent_t *chunkselm;

	if (config_stats)
		nmadvise = 0;
	npurged = 0;

	malloc_mutex_unlock(tsdn, &arena->lock);
	for (rdelm = qr_next(purge_runs_sentinel, rd_link),
	    chunkselm = qr_next(purge_chunks_sentinel, cc_link);
	    rdelm != purge_runs_sentinel; rdelm = qr_next(rdelm, rd_link)) {
		size_t npages;

		if (rdelm == &chunkselm->rd) {
			/*
			 * Don't actually purge the chunk here because 1)
			 * chunkselm is embedded in the chunk and must remain
			 * valid, and 2) we deallocate the chunk in
			 * arena_unstash_purged(), where it is destroyed,
			 * decommitted, or purged, depending on chunk
			 * deallocation policy.
			 */
			size_t size = extent_size_get(chunkselm);
			npages = size >> LG_PAGE;
			chunkselm = qr_next(chunkselm, cc_link);
		} else {
			size_t pageind, run_size, flag_unzeroed, flags, i;
			bool decommitted;
			extent_t *extent = iealloc(tsdn, rdelm);
			arena_chunk_t *chunk =
			    (arena_chunk_t *)extent_addr_get(extent);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(extent, rdelm);
			pageind = arena_miscelm_to_pageind(extent, miscelm);
			run_size = arena_mapbits_large_size_get(chunk, pageind);
			npages = run_size >> LG_PAGE;

			assert(pageind + npages <= chunk_npages);
			assert(!arena_mapbits_decommitted_get(chunk, pageind));
			assert(!arena_mapbits_decommitted_get(chunk,
			    pageind+npages-1));
			decommitted = !chunk_decommit_wrapper(tsdn, arena,
			    chunk_hooks, extent, pageind << LG_PAGE, npages <<
			    LG_PAGE);
			if (decommitted) {
				flag_unzeroed = 0;
				flags = CHUNK_MAP_DECOMMITTED;
			} else {
				flag_unzeroed = chunk_purge_wrapper(tsdn, arena,
				    chunk_hooks, extent, pageind << LG_PAGE,
				    run_size) ? CHUNK_MAP_UNZEROED : 0;
				flags = flag_unzeroed;
			}
			arena_mapbits_large_set(chunk, pageind+npages-1, 0,
			    flags);
			arena_mapbits_large_set(chunk, pageind, run_size,
			    flags);

			/*
			 * Set the unzeroed flag for internal pages, now that
			 * chunk_purge_wrapper() has returned whether the pages
			 * were zeroed as a side effect of purging.  This chunk
			 * map modification is safe even though the arena mutex
			 * isn't currently owned by this thread, because the run
			 * is marked as allocated, thus protecting it from being
			 * modified by any other thread.  As long as these
			 * writes don't perturb the first and last elements'
			 * CHUNK_MAP_ALLOCATED bits, behavior is well defined.
			 */
			for (i = 1; i < npages-1; i++) {
				arena_mapbits_internal_set(chunk, pageind+i,
				    flag_unzeroed);
			}
		}

		npurged += npages;
		if (config_stats)
			nmadvise++;
	}
	malloc_mutex_lock(tsdn, &arena->lock);

	if (config_stats) {
		arena->stats.nmadvise += nmadvise;
		arena->stats.purged += npurged;
	}

	return (npurged);
}

static void
arena_unstash_purged(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    arena_runs_dirty_link_t *purge_runs_sentinel,
    extent_t *purge_chunks_sentinel)
{
	arena_runs_dirty_link_t *rdelm, *rdelm_next;
	extent_t *chunkselm;

	/* Deallocate chunks/runs. */
	for (rdelm = qr_next(purge_runs_sentinel, rd_link),
	    chunkselm = qr_next(purge_chunks_sentinel, cc_link);
	    rdelm != purge_runs_sentinel; rdelm = rdelm_next) {
		rdelm_next = qr_next(rdelm, rd_link);
		if (rdelm == &chunkselm->rd) {
			extent_t *chunkselm_next = qr_next(chunkselm, cc_link);
			extent_dirty_remove(chunkselm);
			chunk_dalloc_wrapper(tsdn, arena, chunk_hooks,
			    chunkselm);
			chunkselm = chunkselm_next;
		} else {
			extent_t *extent = iealloc(tsdn, rdelm);
			arena_chunk_t *chunk =
			    (arena_chunk_t *)extent_addr_get(extent);
			arena_chunk_map_misc_t *miscelm =
			    arena_rd_to_miscelm(extent, rdelm);
			size_t pageind = arena_miscelm_to_pageind(extent,
			    miscelm);
			bool decommitted = (arena_mapbits_decommitted_get(chunk,
			    pageind) != 0);
			arena_run_t *run = &miscelm->run;
			qr_remove(rdelm, rd_link);
			arena_run_dalloc(tsdn, arena, extent, run, false, true,
			    decommitted);
		}
	}
}

/*
 * NB: ndirty_limit is interpreted differently depending on opt_purge:
 *   - purge_mode_ratio: Purge as few dirty run/chunks as possible to reach the
 *                       desired state:
 *                       (arena->ndirty <= ndirty_limit)
 *   - purge_mode_decay: Purge as many dirty runs/chunks as possible without
 *                       violating the invariant:
 *                       (arena->ndirty >= ndirty_limit)
 */
static void
arena_purge_to_limit(tsdn_t *tsdn, arena_t *arena, size_t ndirty_limit)
{
	chunk_hooks_t chunk_hooks = chunk_hooks_get(tsdn, arena);
	size_t npurge, npurged;
	arena_runs_dirty_link_t purge_runs_sentinel;
	extent_t purge_chunks_sentinel;

	arena->purging = true;

	/*
	 * Calls to arena_dirty_count() are disabled even for debug builds
	 * because overhead grows nonlinearly as memory usage increases.
	 */
	if (false && config_debug) {
		size_t ndirty = arena_dirty_count(tsdn, arena);
		assert(ndirty == arena->ndirty);
	}
	assert(opt_purge != purge_mode_ratio || (arena->nactive >>
	    arena->lg_dirty_mult) < arena->ndirty || ndirty_limit == 0);

	qr_new(&purge_runs_sentinel, rd_link);
	extent_init(&purge_chunks_sentinel, arena, NULL, 0, false, false, false,
	    false, false);

	npurge = arena_stash_dirty(tsdn, arena, &chunk_hooks, ndirty_limit,
	    &purge_runs_sentinel, &purge_chunks_sentinel);
	if (npurge == 0)
		goto label_return;
	npurged = arena_purge_stashed(tsdn, arena, &chunk_hooks,
	    &purge_runs_sentinel, &purge_chunks_sentinel);
	assert(npurged == npurge);
	arena_unstash_purged(tsdn, arena, &chunk_hooks, &purge_runs_sentinel,
	    &purge_chunks_sentinel);

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
arena_achunk_prof_reset(tsd_t *tsd, arena_t *arena, extent_t *extent)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	size_t pageind, npages;

	cassert(config_prof);
	assert(opt_prof);

	/*
	 * Iterate over the allocated runs and remove profiled allocations from
	 * the sample set.
	 */
	for (pageind = map_bias; pageind < chunk_npages; pageind += npages) {
		if (arena_mapbits_allocated_get(chunk, pageind) != 0) {
			if (arena_mapbits_large_get(chunk, pageind) != 0) {
				void *ptr = (void *)((uintptr_t)chunk + (pageind
				    << LG_PAGE));
				size_t usize = isalloc(tsd_tsdn(tsd), extent,
				    ptr, config_prof);

				prof_free(tsd, extent, ptr, usize);
				npages = arena_mapbits_large_size_get(chunk,
				    pageind) >> LG_PAGE;
			} else {
				/* Skip small run. */
				size_t binind = arena_mapbits_binind_get(chunk,
				    pageind);
				const arena_bin_info_t *bin_info =
				    &arena_bin_info[binind];
				npages = bin_info->run_size >> LG_PAGE;
			}
		} else {
			/* Skip unallocated run. */
			npages = arena_mapbits_unallocated_size_get(chunk,
			    pageind) >> LG_PAGE;
		}
		assert(pageind + npages <= chunk_npages);
	}
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

	/* Remove large allocations from prof sample set. */
	if (config_prof && opt_prof) {
		ql_foreach(extent, &arena->achunks, ql_link) {
			arena_achunk_prof_reset(tsd, arena, extent);
		}
	}

	/* Reset curruns for large size classes. */
	if (config_stats) {
		for (i = 0; i < nlclasses; i++)
			arena->stats.lstats[i].curruns = 0;
	}

	/* Huge allocations. */
	malloc_mutex_lock(tsd_tsdn(tsd), &arena->huge_mtx);
	for (extent = ql_last(&arena->huge, ql_link); extent != NULL; extent =
	    ql_last(&arena->huge, ql_link)) {
		void *ptr = extent_addr_get(extent);
		size_t usize;

		malloc_mutex_unlock(tsd_tsdn(tsd), &arena->huge_mtx);
		if (config_stats || (config_prof && opt_prof)) {
			usize = isalloc(tsd_tsdn(tsd), extent, ptr,
			    config_prof);
		}
		/* Remove huge allocation from prof sample set. */
		if (config_prof && opt_prof)
			prof_free(tsd, extent, ptr, usize);
		huge_dalloc(tsd_tsdn(tsd), extent);
		malloc_mutex_lock(tsd_tsdn(tsd), &arena->huge_mtx);
		/* Cancel out unwanted effects on stats. */
		if (config_stats)
			arena_huge_reset_stats_cancel(arena, usize);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->huge_mtx);

	malloc_mutex_lock(tsd_tsdn(tsd), &arena->lock);

	/* Bins. */
	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		bin->runcur = NULL;
		arena_run_heap_new(&bin->runs);
		if (config_stats) {
			bin->stats.curregs = 0;
			bin->stats.curruns = 0;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
	}

	/*
	 * Re-initialize runs_dirty such that the chunks_cache and runs_dirty
	 * chains directly correspond.
	 */
	qr_new(&arena->runs_dirty, rd_link);
	for (extent = qr_next(&arena->chunks_cache, cc_link);
	    extent != &arena->chunks_cache; extent = qr_next(extent, cc_link)) {
		qr_new(&extent->rd, rd_link);
		qr_meld(&arena->runs_dirty, &extent->rd, rd_link);
	}

	/* Arena chunks. */
	for (extent = ql_last(&arena->achunks, ql_link); extent != NULL; extent
	    = ql_last(&arena->achunks, ql_link)) {
		ql_remove(&arena->achunks, extent, ql_link);
		arena_chunk_discard(tsd_tsdn(tsd), arena, extent);
	}

	/* Spare. */
	if (arena->spare != NULL) {
		arena_chunk_discard(tsd_tsdn(tsd), arena, arena->spare);
		arena->spare = NULL;
	}

	assert(!arena->purging);
	arena->nactive = 0;

	for (i = 0; i < sizeof(arena->runs_avail) / sizeof(arena_run_heap_t);
	    i++)
		arena_run_heap_new(&arena->runs_avail[i]);

	malloc_mutex_unlock(tsd_tsdn(tsd), &arena->lock);
}

static void
arena_run_coalesce(arena_t *arena, extent_t *extent, size_t *p_size,
    size_t *p_run_ind, size_t *p_run_pages, size_t flag_dirty,
    size_t flag_decommitted)
{
	arena_chunk_t *chunk = (arena_chunk_t *)extent_addr_get(extent);
	size_t size = *p_size;
	size_t run_ind = *p_run_ind;
	size_t run_pages = *p_run_pages;

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    arena_mapbits_allocated_get(chunk, run_ind+run_pages) == 0 &&
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages) == flag_dirty &&
	    arena_mapbits_decommitted_get(chunk, run_ind+run_pages) ==
	    flag_decommitted) {
		size_t nrun_size = arena_mapbits_unallocated_size_get(chunk,
		    run_ind+run_pages);
		size_t nrun_pages = nrun_size >> LG_PAGE;

		/*
		 * Remove successor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert(arena_mapbits_unallocated_size_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == nrun_size);
		assert(arena_mapbits_dirty_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == flag_dirty);
		assert(arena_mapbits_decommitted_get(chunk,
		    run_ind+run_pages+nrun_pages-1) == flag_decommitted);
		arena_avail_remove(arena, extent, run_ind+run_pages,
		    nrun_pages);

		/*
		 * If the successor is dirty, remove it from the set of dirty
		 * pages.
		 */
		if (flag_dirty != 0) {
			arena_run_dirty_remove(arena, chunk, run_ind+run_pages,
			    nrun_pages);
		}

		size += nrun_size;
		run_pages += nrun_pages;

		arena_mapbits_unallocated_size_set(chunk, run_ind, size);
		arena_mapbits_unallocated_size_set(chunk, run_ind+run_pages-1,
		    size);
	}

	/* Try to coalesce backward. */
	if (run_ind > map_bias && arena_mapbits_allocated_get(chunk,
	    run_ind-1) == 0 && arena_mapbits_dirty_get(chunk, run_ind-1) ==
	    flag_dirty && arena_mapbits_decommitted_get(chunk, run_ind-1) ==
	    flag_decommitted) {
		size_t prun_size = arena_mapbits_unallocated_size_get(chunk,
		    run_ind-1);
		size_t prun_pages = prun_size >> LG_PAGE;

		run_ind -= prun_pages;

		/*
		 * Remove predecessor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		assert(arena_mapbits_unallocated_size_get(chunk, run_ind) ==
		    prun_size);
		assert(arena_mapbits_dirty_get(chunk, run_ind) == flag_dirty);
		assert(arena_mapbits_decommitted_get(chunk, run_ind) ==
		    flag_decommitted);
		arena_avail_remove(arena, extent, run_ind, prun_pages);

		/*
		 * If the predecessor is dirty, remove it from the set of dirty
		 * pages.
		 */
		if (flag_dirty != 0) {
			arena_run_dirty_remove(arena, chunk, run_ind,
			    prun_pages);
		}

		size += prun_size;
		run_pages += prun_pages;

		arena_mapbits_unallocated_size_set(chunk, run_ind, size);
		arena_mapbits_unallocated_size_set(chunk, run_ind+run_pages-1,
		    size);
	}

	*p_size = size;
	*p_run_ind = run_ind;
	*p_run_pages = run_pages;
}

static size_t
arena_run_size_get(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t run_ind)
{
	size_t size;

	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);

	if (arena_mapbits_large_get(chunk, run_ind) != 0) {
		size = arena_mapbits_large_size_get(chunk, run_ind);
		assert(size == PAGE || arena_mapbits_large_size_get(chunk,
		    run_ind+(size>>LG_PAGE)-1) == 0);
	} else {
		const arena_bin_info_t *bin_info = &arena_bin_info[run->binind];
		size = bin_info->run_size;
	}

	return (size);
}

static void
arena_run_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, bool dirty, bool cleaned, bool decommitted)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t size, run_ind, run_pages, flag_dirty, flag_decommitted;

	chunk = (arena_chunk_t *)extent_addr_get(extent);
	miscelm = arena_run_to_miscelm(extent, run);
	run_ind = arena_miscelm_to_pageind(extent, miscelm);
	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);
	size = arena_run_size_get(arena, chunk, run, run_ind);
	run_pages = (size >> LG_PAGE);
	arena_nactive_sub(arena, run_pages);

	/*
	 * The run is dirty if the caller claims to have dirtied it, as well as
	 * if it was already dirty before being allocated and the caller
	 * doesn't claim to have cleaned it.
	 */
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	if (!cleaned && !decommitted && arena_mapbits_dirty_get(chunk, run_ind)
	    != 0)
		dirty = true;
	flag_dirty = dirty ? CHUNK_MAP_DIRTY : 0;
	flag_decommitted = decommitted ? CHUNK_MAP_DECOMMITTED : 0;

	/* Mark pages as unallocated in the chunk map. */
	if (dirty || decommitted) {
		size_t flags = flag_dirty | flag_decommitted;
		arena_mapbits_unallocated_set(chunk, run_ind, size, flags);
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    flags);
	} else {
		arena_mapbits_unallocated_set(chunk, run_ind, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind));
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind+run_pages-1));
	}

	arena_run_coalesce(arena, extent, &size, &run_ind, &run_pages,
	    flag_dirty, flag_decommitted);

	/* Insert into runs_avail, now that coalescing is complete. */
	assert(arena_mapbits_unallocated_size_get(chunk, run_ind) ==
	    arena_mapbits_unallocated_size_get(chunk, run_ind+run_pages-1));
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	assert(arena_mapbits_decommitted_get(chunk, run_ind) ==
	    arena_mapbits_decommitted_get(chunk, run_ind+run_pages-1));
	arena_avail_insert(arena, extent, run_ind, run_pages);

	if (dirty)
		arena_run_dirty_insert(arena, chunk, run_ind, run_pages);

	/* Deallocate chunk if it is now completely unused. */
	if (size == arena_maxrun) {
		assert(run_ind == map_bias);
		assert(run_pages == (arena_maxrun >> LG_PAGE));
		arena_chunk_dalloc(tsdn, arena, extent);
	}

	/*
	 * It is okay to do dirty page processing here even if the chunk was
	 * deallocated above, since in that case it is the spare.  Waiting
	 * until after possible chunk deallocation to do dirty processing
	 * allows for an old spare to be fully deallocated, thus decreasing the
	 * chances of spuriously crossing the dirty page purging threshold.
	 */
	if (dirty)
		arena_maybe_purge(tsdn, arena);
}

static void
arena_run_trim_head(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, arena_run_t *run, size_t oldsize, size_t newsize)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(extent, run);
	size_t pageind = arena_miscelm_to_pageind(extent, miscelm);
	size_t head_npages = (oldsize - newsize) >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);
	size_t flag_decommitted = arena_mapbits_decommitted_get(chunk, pageind);
	size_t flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages-1)));
	arena_mapbits_large_set(chunk, pageind, oldsize-newsize, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, pageind)));

	if (config_debug) {
		UNUSED size_t tail_npages = newsize >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, newsize,
	    flag_dirty | (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages)));

	arena_run_dalloc(tsdn, arena, extent, run, false, false,
	    (flag_decommitted != 0));
}

static void
arena_run_trim_tail(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, arena_run_t *run, size_t oldsize, size_t newsize,
    bool dirty)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(extent, run);
	size_t pageind = arena_miscelm_to_pageind(extent, miscelm);
	size_t head_npages = newsize >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);
	size_t flag_decommitted = arena_mapbits_decommitted_get(chunk, pageind);
	size_t flag_unzeroed_mask = (flag_dirty | flag_decommitted) == 0 ?
	    CHUNK_MAP_UNZEROED : 0;
	arena_chunk_map_misc_t *tail_miscelm;
	arena_run_t *tail_run;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages-1)));
	arena_mapbits_large_set(chunk, pageind, newsize, flag_dirty |
	    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk, pageind)));

	if (config_debug) {
		UNUSED size_t tail_npages = (oldsize - newsize) >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, oldsize-newsize,
	    flag_dirty | (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
	    pageind+head_npages)));

	tail_miscelm = arena_miscelm_get_mutable(chunk, pageind + head_npages);
	tail_run = &tail_miscelm->run;
	arena_run_dalloc(tsdn, arena, extent, tail_run, dirty, false,
	    (flag_decommitted != 0));
}

static void
arena_bin_runs_insert(arena_bin_t *bin, extent_t *extent, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(extent, run);

	arena_run_heap_insert(&bin->runs, miscelm);
}

static arena_run_t *
arena_bin_nonfull_run_tryget(arena_bin_t *bin)
{
	arena_chunk_map_misc_t *miscelm;

	miscelm = arena_run_heap_remove_first(&bin->runs);
	if (miscelm == NULL)
		return (NULL);
	if (config_stats)
		bin->stats.reruns++;

	return (&miscelm->run);
}

static arena_run_t *
arena_bin_nonfull_run_get(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin)
{
	arena_run_t *run;
	szind_t binind;
	const arena_bin_info_t *bin_info;

	/* Look for a usable run. */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);
	/* No existing runs have any space available. */

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];

	/* Allocate a new run. */
	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	malloc_mutex_lock(tsdn, &arena->lock);
	run = arena_run_alloc_small(tsdn, arena, bin_info->run_size, binind);
	if (run != NULL) {
		/* Initialize run internals. */
		run->binind = binind;
		run->nfree = bin_info->nregs;
		bitmap_init(run->bitmap, &bin_info->bitmap_info);
	}
	malloc_mutex_unlock(tsdn, &arena->lock);
	/********************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (run != NULL) {
		if (config_stats) {
			bin->stats.nruns++;
			bin->stats.curruns++;
		}
		return (run);
	}

	/*
	 * arena_run_alloc_small() failed, but another thread may have made
	 * sufficient memory available while this one dropped bin->lock above,
	 * so search one more time.
	 */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);

	return (NULL);
}

/* Re-fill bin->runcur, then call arena_run_reg_alloc(). */
static void *
arena_bin_malloc_hard(tsdn_t *tsdn, arena_t *arena, arena_bin_t *bin)
{
	szind_t binind;
	const arena_bin_info_t *bin_info;
	arena_run_t *run;

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];
	bin->runcur = NULL;
	run = arena_bin_nonfull_run_get(tsdn, arena, bin);
	if (bin->runcur != NULL && bin->runcur->nfree > 0) {
		/*
		 * Another thread updated runcur while this one ran without the
		 * bin lock in arena_bin_nonfull_run_get().
		 */
		void *ret;
		assert(bin->runcur->nfree > 0);
		ret = arena_run_reg_alloc(tsdn, bin->runcur, bin_info);
		if (run != NULL) {
			extent_t *extent;
			arena_chunk_t *chunk;

			/*
			 * arena_run_alloc_small() may have allocated run, or
			 * it may have pulled run from the bin's run tree.
			 * Therefore it is unsafe to make any assumptions about
			 * how run has previously been used, and
			 * arena_bin_lower_run() must be called, as if a region
			 * were just deallocated from the run.
			 */
			extent = iealloc(tsdn, run);
			chunk = (arena_chunk_t *)extent_addr_get(extent);
			if (run->nfree == bin_info->nregs) {
				arena_dalloc_bin_run(tsdn, arena, chunk, extent,
				    run, bin);
			} else {
				arena_bin_lower_run(tsdn, arena, extent, run,
				    bin);
			}
		}
		return (ret);
	}

	if (run == NULL)
		return (NULL);

	bin->runcur = run;

	assert(bin->runcur->nfree > 0);

	return (arena_run_reg_alloc(tsdn, bin->runcur, bin_info));
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
		arena_run_t *run;
		void *ptr;
		if ((run = bin->runcur) != NULL && run->nfree > 0) {
			ptr = arena_run_reg_alloc(tsdn, run,
			    &arena_bin_info[binind]);
		} else
			ptr = arena_bin_malloc_hard(tsdn, arena, bin);
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
	arena_run_t *run;

	assert(binind < NBINS);
	bin = &arena->bins[binind];
	usize = index2size(binind);

	malloc_mutex_lock(tsdn, &bin->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_run_reg_alloc(tsdn, run, &arena_bin_info[binind]);
	else
		ret = arena_bin_malloc_hard(tsdn, arena, bin);

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
	if (config_prof && !isthreaded && arena_prof_accum(tsdn, arena, usize))
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
arena_malloc_large(tsdn_t *tsdn, arena_t *arena, szind_t binind, bool zero)
{
	void *ret;
	size_t usize;
	uintptr_t random_offset;
	arena_run_t *run;
	extent_t *extent;
	arena_chunk_map_misc_t *miscelm;
	UNUSED bool idump JEMALLOC_CC_SILENCE_INIT(false);

	/* Large allocation. */
	usize = index2size(binind);
	malloc_mutex_lock(tsdn, &arena->lock);
	if (config_cache_oblivious) {
		uint64_t r;

		/*
		 * Compute a uniformly distributed offset within the first page
		 * that is a multiple of the cacheline size, e.g. [0 .. 63) * 64
		 * for 4 KiB pages and 64-byte cachelines.
		 */
		r = prng_lg_range(&arena->offset_state, LG_PAGE - LG_CACHELINE);
		random_offset = ((uintptr_t)r) << LG_CACHELINE;
	} else
		random_offset = 0;
	run = arena_run_alloc_large(tsdn, arena, usize + large_pad, zero);
	if (run == NULL) {
		malloc_mutex_unlock(tsdn, &arena->lock);
		return (NULL);
	}
	extent = iealloc(tsdn, run);
	miscelm = arena_run_to_miscelm(extent, run);
	ret = (void *)((uintptr_t)arena_miscelm_to_rpages(extent, miscelm) +
	    random_offset);
	if (config_stats) {
		szind_t index = binind - NBINS;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += usize;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	if (config_prof)
		idump = arena_prof_accum_locked(arena, usize);
	malloc_mutex_unlock(tsdn, &arena->lock);
	if (config_prof && idump)
		prof_idump(tsdn);

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk_alloc))
				memset(ret, JEMALLOC_ALLOC_JUNK, usize);
			else if (unlikely(opt_zero))
				memset(ret, 0, usize);
		}
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
	if (likely(size <= large_maxclass))
		return (arena_malloc_large(tsdn, arena, ind, zero));
	return (huge_malloc(tsdn, arena, index2size(ind), zero));
}

/* Only handles large allocations that require more than page alignment. */
static void *
arena_palloc_large(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	void *ret;
	size_t alloc_size, leadsize, trailsize;
	arena_run_t *run;
	extent_t *extent;
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(!tsdn_null(tsdn) || arena != NULL);
	assert(usize == PAGE_CEILING(usize));

	if (likely(!tsdn_null(tsdn)))
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	if (unlikely(arena == NULL))
		return (NULL);

	alignment = PAGE_CEILING(alignment);
	alloc_size = usize + large_pad + alignment;

	malloc_mutex_lock(tsdn, &arena->lock);
	run = arena_run_alloc_large(tsdn, arena, alloc_size, false);
	if (run == NULL) {
		malloc_mutex_unlock(tsdn, &arena->lock);
		return (NULL);
	}
	extent = iealloc(tsdn, run);
	chunk = (arena_chunk_t *)extent_addr_get(extent);
	miscelm = arena_run_to_miscelm(extent, run);
	rpages = arena_miscelm_to_rpages(extent, miscelm);

	leadsize = ALIGNMENT_CEILING((uintptr_t)rpages, alignment) -
	    (uintptr_t)rpages;
	assert(alloc_size >= leadsize + usize);
	trailsize = alloc_size - leadsize - usize - large_pad;
	if (leadsize != 0) {
		arena_chunk_map_misc_t *head_miscelm = miscelm;
		arena_run_t *head_run = run;
		extent_t *head_extent = extent;

		miscelm = arena_miscelm_get_mutable(chunk,
		    arena_miscelm_to_pageind(head_extent, head_miscelm) +
		    (leadsize >> LG_PAGE));
		run = &miscelm->run;
		extent = iealloc(tsdn, run);

		arena_run_trim_head(tsdn, arena, chunk, head_extent, head_run,
		    alloc_size, alloc_size - leadsize);
	}
	if (trailsize != 0) {
		arena_run_trim_tail(tsdn, arena, chunk, extent, run, usize +
		    large_pad + trailsize, usize + large_pad, false);
	}
	if (arena_run_init_large(tsdn, arena, extent, run, usize + large_pad,
	    zero)) {
		size_t run_ind = arena_miscelm_to_pageind(extent,
		    arena_run_to_miscelm(extent, run));
		bool dirty = (arena_mapbits_dirty_get(chunk, run_ind) != 0);
		bool decommitted = (arena_mapbits_decommitted_get(chunk,
		    run_ind) != 0);

		assert(decommitted); /* Cause of OOM. */
		arena_run_dalloc(tsdn, arena, extent, run, dirty, false,
		    decommitted);
		malloc_mutex_unlock(tsdn, &arena->lock);
		return (NULL);
	}
	ret = arena_miscelm_to_rpages(extent, miscelm);

	if (config_stats) {
		szind_t index = size2index(usize) - NBINS;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += usize;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	malloc_mutex_unlock(tsdn, &arena->lock);

	if (config_fill && !zero) {
		if (unlikely(opt_junk_alloc))
			memset(ret, JEMALLOC_ALLOC_JUNK, usize);
		else if (unlikely(opt_zero))
			memset(ret, 0, usize);
	}
	arena_decay_tick(tsdn, arena);
	return (ret);
}

void *
arena_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero, tcache_t *tcache)
{
	void *ret;

	if (usize <= SMALL_MAXCLASS && (alignment < PAGE || (alignment == PAGE
	    && (usize & PAGE_MASK) == 0))) {
		/* Small; alignment doesn't require special run placement. */
		ret = arena_malloc(tsdn, arena, usize, size2index(usize), zero,
		    tcache, true);
	} else if (usize <= large_maxclass && alignment <= PAGE) {
		/*
		 * Large; alignment doesn't require special run placement.
		 * However, the cached pointer may be at a random offset from
		 * the base of the run, so do some bit manipulation to retrieve
		 * the base.
		 */
		ret = arena_malloc(tsdn, arena, usize, size2index(usize), zero,
		    tcache, true);
		if (config_cache_oblivious)
			ret = (void *)((uintptr_t)ret & ~PAGE_MASK);
	} else {
		if (likely(usize <= large_maxclass)) {
			ret = arena_palloc_large(tsdn, arena, usize, alignment,
			    zero);
		} else if (likely(alignment <= PAGE))
			ret = huge_malloc(tsdn, arena, usize, zero);
		else
			ret = huge_palloc(tsdn, arena, usize, alignment, zero);
	}
	return (ret);
}

void
arena_prof_promoted(tsdn_t *tsdn, const extent_t *extent, const void *ptr,
    size_t size)
{
	arena_chunk_t *chunk;
	size_t pageind;
	szind_t binind;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(extent_addr_get(extent) != ptr);
	assert(isalloc(tsdn, extent, ptr, false) == LARGE_MINCLASS);
	assert(isalloc(tsdn, extent, ptr, true) == LARGE_MINCLASS);
	assert(size <= SMALL_MAXCLASS);

	chunk = (arena_chunk_t *)extent_addr_get(extent);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	binind = size2index(size);
	assert(binind < NBINS);
	arena_mapbits_large_binind_set(chunk, pageind, binind);

	assert(isalloc(tsdn, extent, ptr, false) == LARGE_MINCLASS);
	assert(isalloc(tsdn, extent, ptr, true) == size);
}

static void
arena_dissociate_bin_run(extent_t *extent, arena_run_t *run, arena_bin_t *bin)
{

	/* Dissociate run from bin. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else {
		szind_t binind = arena_bin_index(extent_arena_get(extent), bin);
		const arena_bin_info_t *bin_info = &arena_bin_info[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * run only contains one region, then it never gets inserted
		 * into the non-full runs tree.
		 */
		if (bin_info->nregs != 1) {
			arena_chunk_map_misc_t *miscelm =
			    arena_run_to_miscelm(extent, run);

			arena_run_heap_remove(&bin->runs, miscelm);
		}
	}
}

static void
arena_dalloc_bin_run(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, arena_run_t *run, arena_bin_t *bin)
{

	assert(run != bin->runcur);

	malloc_mutex_unlock(tsdn, &bin->lock);
	/******************************/
	malloc_mutex_lock(tsdn, &arena->lock);
	arena_run_dalloc(tsdn, arena, extent, run, true, false, false);
	malloc_mutex_unlock(tsdn, &arena->lock);
	/****************************/
	malloc_mutex_lock(tsdn, &bin->lock);
	if (config_stats)
		bin->stats.curruns--;
}

static void
arena_bin_lower_run(tsdn_t *tsdn, arena_t *arena, extent_t *extent,
    arena_run_t *run, arena_bin_t *bin)
{

	/*
	 * Make sure that if bin->runcur is non-NULL, it refers to the lowest
	 * non-full run.  It is okay to NULL runcur out rather than proactively
	 * keeping it pointing at the lowest non-full run.
	 */
	if ((uintptr_t)run < (uintptr_t)bin->runcur) {
		/* Switch runcur. */
		if (bin->runcur->nfree > 0) {
			arena_bin_runs_insert(bin, iealloc(tsdn, bin->runcur),
			    bin->runcur);
		}
		bin->runcur = run;
		if (config_stats)
			bin->stats.reruns++;
	} else
		arena_bin_runs_insert(bin, extent, run);
}

static void
arena_dalloc_bin_locked_impl(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, void *ptr, arena_chunk_map_bits_t *bitselm, bool junked)
{
	size_t pageind, rpages_ind;
	arena_run_t *run;
	arena_bin_t *bin;
	const arena_bin_info_t *bin_info;
	szind_t binind;

	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get_mutable(chunk, rpages_ind)->run;
	binind = run->binind;
	bin = &arena->bins[binind];
	bin_info = &arena_bin_info[binind];

	if (!junked && config_fill && unlikely(opt_junk_free))
		arena_dalloc_junk_small(ptr, bin_info);

	arena_run_reg_dalloc(tsdn, run, extent, ptr);
	if (run->nfree == bin_info->nregs) {
		arena_dissociate_bin_run(extent, run, bin);
		arena_dalloc_bin_run(tsdn, arena, chunk, extent, run, bin);
	} else if (run->nfree == 1 && run != bin->runcur)
		arena_bin_lower_run(tsdn, arena, extent, run, bin);

	if (config_stats) {
		bin->stats.ndalloc++;
		bin->stats.curregs--;
	}
}

void
arena_dalloc_bin_junked_locked(tsdn_t *tsdn, arena_t *arena,
    arena_chunk_t *chunk, extent_t *extent, void *ptr,
    arena_chunk_map_bits_t *bitselm)
{

	arena_dalloc_bin_locked_impl(tsdn, arena, chunk, extent, ptr, bitselm,
	    true);
}

static void
arena_dalloc_bin(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, void *ptr, size_t pageind, arena_chunk_map_bits_t *bitselm)
{
	arena_run_t *run;
	arena_bin_t *bin;
	size_t rpages_ind;

	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get_mutable(chunk, rpages_ind)->run;
	bin = &arena->bins[run->binind];
	malloc_mutex_lock(tsdn, &bin->lock);
	arena_dalloc_bin_locked_impl(tsdn, arena, chunk, extent, ptr, bitselm,
	    false);
	malloc_mutex_unlock(tsdn, &bin->lock);
}

void
arena_dalloc_small(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, void *ptr, size_t pageind)
{
	arena_chunk_map_bits_t *bitselm;

	if (config_debug) {
		/* arena_ptr_small_binind_get() does extra sanity checking. */
		assert(arena_ptr_small_binind_get(tsdn, ptr,
		    arena_mapbits_get(chunk, pageind)) != BININD_INVALID);
	}
	bitselm = arena_bitselm_get_mutable(chunk, pageind);
	arena_dalloc_bin(tsdn, arena, chunk, extent, ptr, pageind, bitselm);
	arena_decay_tick(tsdn, arena);
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(n_arena_dalloc_junk_large)
#endif
void
arena_dalloc_junk_large(void *ptr, size_t usize)
{

	if (config_fill && unlikely(opt_junk_free))
		memset(ptr, JEMALLOC_FREE_JUNK, usize);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(arena_dalloc_junk_large)
arena_dalloc_junk_large_t *arena_dalloc_junk_large =
    JEMALLOC_N(n_arena_dalloc_junk_large);
#endif

static void
arena_dalloc_large_locked_impl(tsdn_t *tsdn, arena_t *arena,
    arena_chunk_t *chunk, extent_t *extent, void *ptr, bool junked)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get_mutable(chunk,
	    pageind);
	arena_run_t *run = &miscelm->run;

	if (config_fill || config_stats) {
		size_t usize = arena_mapbits_large_size_get(chunk, pageind) -
		    large_pad;

		if (!junked)
			arena_dalloc_junk_large(ptr, usize);
		if (config_stats) {
			szind_t index = size2index(usize) - NBINS;

			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= usize;
			arena->stats.lstats[index].ndalloc++;
			arena->stats.lstats[index].curruns--;
		}
	}

	arena_run_dalloc(tsdn, arena, extent, run, true, false, false);
}

void
arena_dalloc_large_junked_locked(tsdn_t *tsdn, arena_t *arena,
    arena_chunk_t *chunk, extent_t *extent, void *ptr)
{

	arena_dalloc_large_locked_impl(tsdn, arena, chunk, extent, ptr, true);
}

void
arena_dalloc_large(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, void *ptr)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_dalloc_large_locked_impl(tsdn, arena, chunk, extent, ptr, false);
	malloc_mutex_unlock(tsdn, &arena->lock);
	arena_decay_tick(tsdn, arena);
}

static void
arena_ralloc_large_shrink(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    extent_t *extent, void *ptr, size_t oldsize, size_t size)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get_mutable(chunk,
	    pageind);
	arena_run_t *run = &miscelm->run;

	assert(size < oldsize);

	/*
	 * Shrink the run, and make trailing pages available for other
	 * allocations.
	 */
	malloc_mutex_lock(tsdn, &arena->lock);
	arena_run_trim_tail(tsdn, arena, chunk, extent, run, oldsize +
	    large_pad, size + large_pad, true);
	if (config_stats) {
		szind_t oldindex = size2index(oldsize) - NBINS;
		szind_t index = size2index(size) - NBINS;

		arena->stats.ndalloc_large++;
		arena->stats.allocated_large -= oldsize;
		arena->stats.lstats[oldindex].ndalloc++;
		arena->stats.lstats[oldindex].curruns--;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[index].nmalloc++;
		arena->stats.lstats[index].nrequests++;
		arena->stats.lstats[index].curruns++;
	}
	malloc_mutex_unlock(tsdn, &arena->lock);
}

static bool
arena_ralloc_large_grow(tsdn_t *tsdn, arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t oldsize, size_t usize_min, size_t usize_max, bool zero)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t npages = (oldsize + large_pad) >> LG_PAGE;
	size_t followsize;

	assert(oldsize == arena_mapbits_large_size_get(chunk, pageind) -
	    large_pad);

	/* Try to extend the run. */
	malloc_mutex_lock(tsdn, &arena->lock);
	if (pageind+npages >= chunk_npages || arena_mapbits_allocated_get(chunk,
	    pageind+npages) != 0)
		goto label_fail;
	followsize = arena_mapbits_unallocated_size_get(chunk, pageind+npages);
	if (oldsize + followsize >= usize_min) {
		/*
		 * The next run is available and sufficiently large.  Split the
		 * following run, then merge the first part with the existing
		 * allocation.
		 */
		arena_run_t *run;
		size_t usize, splitsize, size, flag_dirty, flag_unzeroed_mask;

		usize = usize_max;
		while (oldsize + followsize < usize)
			usize = index2size(size2index(usize)-1);
		assert(usize >= usize_min);
		assert(usize >= oldsize);
		splitsize = usize - oldsize;
		if (splitsize == 0)
			goto label_fail;

		run = &arena_miscelm_get_mutable(chunk, pageind+npages)->run;
		if (arena_run_split_large(tsdn, arena, iealloc(tsdn, run), run,
		    splitsize, zero))
			goto label_fail;

		if (config_cache_oblivious && zero) {
			/*
			 * Zero the trailing bytes of the original allocation's
			 * last page, since they are in an indeterminate state.
			 * There will always be trailing bytes, because ptr's
			 * offset from the beginning of the run is a multiple of
			 * CACHELINE in [0 .. PAGE).
			 */
			void *zbase = (void *)((uintptr_t)ptr + oldsize);
			void *zpast = PAGE_ADDR2BASE((void *)((uintptr_t)zbase +
			    PAGE));
			size_t nzero = (uintptr_t)zpast - (uintptr_t)zbase;
			assert(nzero > 0);
			memset(zbase, 0, nzero);
		}

		size = oldsize + splitsize;
		npages = (size + large_pad) >> LG_PAGE;

		/*
		 * Mark the extended run as dirty if either portion of the run
		 * was dirty before allocation.  This is rather pedantic,
		 * because there's not actually any sequence of events that
		 * could cause the resulting run to be passed to
		 * arena_run_dalloc() with the dirty argument set to false
		 * (which is when dirty flag consistency would really matter).
		 */
		flag_dirty = arena_mapbits_dirty_get(chunk, pageind) |
		    arena_mapbits_dirty_get(chunk, pageind+npages-1);
		flag_unzeroed_mask = flag_dirty == 0 ? CHUNK_MAP_UNZEROED : 0;
		arena_mapbits_large_set(chunk, pageind, size + large_pad,
		    flag_dirty | (flag_unzeroed_mask &
		    arena_mapbits_unzeroed_get(chunk, pageind)));
		arena_mapbits_large_set(chunk, pageind+npages-1, 0, flag_dirty |
		    (flag_unzeroed_mask & arena_mapbits_unzeroed_get(chunk,
		    pageind+npages-1)));

		if (config_stats) {
			szind_t oldindex = size2index(oldsize) - NBINS;
			szind_t index = size2index(size) - NBINS;

			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= oldsize;
			arena->stats.lstats[oldindex].ndalloc++;
			arena->stats.lstats[oldindex].curruns--;

			arena->stats.nmalloc_large++;
			arena->stats.nrequests_large++;
			arena->stats.allocated_large += size;
			arena->stats.lstats[index].nmalloc++;
			arena->stats.lstats[index].nrequests++;
			arena->stats.lstats[index].curruns++;
		}
		malloc_mutex_unlock(tsdn, &arena->lock);
		return (false);
	}
label_fail:
	malloc_mutex_unlock(tsdn, &arena->lock);
	return (true);
}

#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(n_arena_ralloc_junk_large)
#endif
static void
arena_ralloc_junk_large(void *ptr, size_t old_usize, size_t usize)
{

	if (config_fill && unlikely(opt_junk_free)) {
		memset((void *)((uintptr_t)ptr + usize), JEMALLOC_FREE_JUNK,
		    old_usize - usize);
	}
}
#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(arena_ralloc_junk_large)
arena_ralloc_junk_large_t *arena_ralloc_junk_large =
    JEMALLOC_N(n_arena_ralloc_junk_large);
#endif

/*
 * Try to resize a large allocation, in order to avoid copying.  This will
 * always fail if growing an object, and the following run is already in use.
 */
static bool
arena_ralloc_large(tsdn_t *tsdn, extent_t *extent, void *ptr, size_t oldsize,
    size_t usize_min, size_t usize_max, bool zero)
{
	arena_chunk_t *chunk;
	arena_t *arena;

	if (oldsize == usize_max) {
		/* Current size class is compatible and maximal. */
		return (false);
	}

	chunk = (arena_chunk_t *)extent_addr_get(extent);
	arena = extent_arena_get(extent);

	if (oldsize < usize_max) {
		bool ret = arena_ralloc_large_grow(tsdn, arena, chunk, ptr,
		    oldsize, usize_min, usize_max, zero);
		if (config_fill && !ret && !zero) {
			if (unlikely(opt_junk_alloc)) {
				memset((void *)((uintptr_t)ptr + oldsize),
				    JEMALLOC_ALLOC_JUNK,
				    isalloc(tsdn, extent, ptr, config_prof) -
				    oldsize);
			} else if (unlikely(opt_zero)) {
				memset((void *)((uintptr_t)ptr + oldsize), 0,
				    isalloc(tsdn, extent, ptr, config_prof) -
				    oldsize);
			}
		}
		return (ret);
	}

	assert(oldsize > usize_max);
	/* Fill before shrinking in order avoid a race. */
	arena_ralloc_junk_large(ptr, oldsize, usize_max);
	arena_ralloc_large_shrink(tsdn, arena, chunk, extent, ptr, oldsize,
	    usize_max);
	return (false);
}

bool
arena_ralloc_no_move(tsdn_t *tsdn, extent_t *extent, void *ptr, size_t oldsize,
    size_t size, size_t extra, bool zero)
{
	size_t usize_min, usize_max;

	/* Calls with non-zero extra had to clamp extra. */
	assert(extra == 0 || size + extra <= HUGE_MAXCLASS);

	if (unlikely(size > HUGE_MAXCLASS))
		return (true);

	usize_min = s2u(size);
	usize_max = s2u(size + extra);
	if (likely(oldsize <= large_maxclass && usize_min <= large_maxclass)) {
		/*
		 * Avoid moving the allocation if the size class can be left the
		 * same.
		 */
		if (oldsize <= SMALL_MAXCLASS) {
			assert(arena_bin_info[size2index(oldsize)].reg_size ==
			    oldsize);
			if ((usize_max > SMALL_MAXCLASS ||
			    size2index(usize_max) != size2index(oldsize)) &&
			    (size > oldsize || usize_max < oldsize))
				return (true);
		} else {
			if (usize_max <= SMALL_MAXCLASS)
				return (true);
			if (arena_ralloc_large(tsdn, extent, ptr, oldsize,
			    usize_min, usize_max, zero))
				return (true);
		}

		arena_decay_tick(tsdn, extent_arena_get(extent));
		return (false);
	} else if (oldsize >= chunksize && usize_max >= chunksize) {
		return (huge_ralloc_no_move(tsdn, extent, usize_min, usize_max,
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
	if (unlikely(usize == 0 || usize > HUGE_MAXCLASS))
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
	if (unlikely(usize == 0 || size > HUGE_MAXCLASS))
		return (NULL);

	if (likely(usize <= large_maxclass)) {
		/* Try to avoid moving the allocation. */
		if (!arena_ralloc_no_move(tsdn, extent, ptr, oldsize, usize, 0,
		    zero))
			return (ptr);
	}

	if (oldsize >= chunksize && usize >= chunksize) {
		return (huge_ralloc(tsdn, arena, extent, usize, alignment, zero,
		    tcache));
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
arena_lg_dirty_mult_default_get(void)
{

	return ((ssize_t)atomic_read_z((size_t *)&lg_dirty_mult_default));
}

bool
arena_lg_dirty_mult_default_set(ssize_t lg_dirty_mult)
{

	if (opt_purge != purge_mode_ratio)
		return (true);
	if (!arena_lg_dirty_mult_valid(lg_dirty_mult))
		return (true);
	atomic_write_z((size_t *)&lg_dirty_mult_default, (size_t)lg_dirty_mult);
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

	if (opt_purge != purge_mode_decay)
		return (true);
	if (!arena_decay_time_valid(decay_time))
		return (true);
	atomic_write_z((size_t *)&decay_time_default, (size_t)decay_time);
	return (false);
}

static void
arena_basic_stats_merge_locked(arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *lg_dirty_mult, ssize_t *decay_time,
    size_t *nactive, size_t *ndirty)
{

	*nthreads += arena_nthreads_get(arena, false);
	*dss = dss_prec_names[arena->dss_prec];
	*lg_dirty_mult = arena->lg_dirty_mult;
	*decay_time = arena->decay_time;
	*nactive += arena->nactive;
	*ndirty += arena->ndirty;
}

void
arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *lg_dirty_mult, ssize_t *decay_time,
    size_t *nactive, size_t *ndirty)
{

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_basic_stats_merge_locked(arena, nthreads, dss, lg_dirty_mult,
	    decay_time, nactive, ndirty);
	malloc_mutex_unlock(tsdn, &arena->lock);
}

void
arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *lg_dirty_mult, ssize_t *decay_time,
    size_t *nactive, size_t *ndirty, arena_stats_t *astats,
    malloc_bin_stats_t *bstats, malloc_large_stats_t *lstats,
    malloc_huge_stats_t *hstats)
{
	unsigned i;

	cassert(config_stats);

	malloc_mutex_lock(tsdn, &arena->lock);
	arena_basic_stats_merge_locked(arena, nthreads, dss, lg_dirty_mult,
	    decay_time, nactive, ndirty);

	astats->mapped += arena->stats.mapped;
	astats->retained += arena->stats.retained;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->metadata_mapped += arena->stats.metadata_mapped;
	astats->metadata_allocated += arena_metadata_allocated_get(arena);
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;
	astats->nrequests_large += arena->stats.nrequests_large;
	astats->allocated_huge += arena->stats.allocated_huge;
	astats->nmalloc_huge += arena->stats.nmalloc_huge;
	astats->ndalloc_huge += arena->stats.ndalloc_huge;

	for (i = 0; i < nlclasses; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}

	for (i = 0; i < nhclasses; i++) {
		hstats[i].nmalloc += arena->stats.hstats[i].nmalloc;
		hstats[i].ndalloc += arena->stats.hstats[i].ndalloc;
		hstats[i].curhchunks += arena->stats.hstats[i].curhchunks;
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
		bstats[i].nruns += bin->stats.nruns;
		bstats[i].reruns += bin->stats.reruns;
		bstats[i].curruns += bin->stats.curruns;
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

	/*
	 * Allocate arena, arena->lstats, and arena->hstats contiguously, mainly
	 * because there is no way to clean up if base_alloc() OOMs.
	 */
	if (config_stats) {
		arena = (arena_t *)base_alloc(tsdn,
		    CACHELINE_CEILING(sizeof(arena_t)) +
		    QUANTUM_CEILING((nlclasses * sizeof(malloc_large_stats_t)) +
		    (nhclasses * sizeof(malloc_huge_stats_t))));
	} else
		arena = (arena_t *)base_alloc(tsdn, sizeof(arena_t));
	if (arena == NULL)
		return (NULL);

	arena->ind = ind;
	arena->nthreads[0] = arena->nthreads[1] = 0;
	if (malloc_mutex_init(&arena->lock, "arena", WITNESS_RANK_ARENA))
		return (NULL);

	if (config_stats) {
		memset(&arena->stats, 0, sizeof(arena_stats_t));
		arena->stats.lstats = (malloc_large_stats_t *)((uintptr_t)arena
		    + CACHELINE_CEILING(sizeof(arena_t)));
		memset(arena->stats.lstats, 0, nlclasses *
		    sizeof(malloc_large_stats_t));
		arena->stats.hstats = (malloc_huge_stats_t *)((uintptr_t)arena
		    + CACHELINE_CEILING(sizeof(arena_t)) +
		    QUANTUM_CEILING(nlclasses * sizeof(malloc_large_stats_t)));
		memset(arena->stats.hstats, 0, nhclasses *
		    sizeof(malloc_huge_stats_t));
		if (config_tcache)
			ql_new(&arena->tcache_ql);
	}

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

	arena->dss_prec = chunk_dss_prec_get(tsdn);

	ql_new(&arena->achunks);

	arena->spare = NULL;

	arena->lg_dirty_mult = arena_lg_dirty_mult_default_get();
	arena->purging = false;
	arena->nactive = 0;
	arena->ndirty = 0;

	qr_new(&arena->runs_dirty, rd_link);
	qr_new(&arena->chunks_cache, cc_link);

	if (opt_purge == purge_mode_decay)
		arena_decay_init(arena, arena_decay_time_default_get());

	ql_new(&arena->huge);
	if (malloc_mutex_init(&arena->huge_mtx, "arena_huge",
	    WITNESS_RANK_ARENA_HUGE))
		return (NULL);

	for (i = 0; i < NPSIZES; i++) {
		extent_heap_new(&arena->chunks_cached[i]);
		extent_heap_new(&arena->chunks_retained[i]);
	}

	if (malloc_mutex_init(&arena->chunks_mtx, "arena_chunks",
	    WITNESS_RANK_ARENA_CHUNKS))
		return (NULL);
	ql_new(&arena->extent_cache);
	if (malloc_mutex_init(&arena->extent_cache_mtx, "arena_extent_cache",
	    WITNESS_RANK_ARENA_EXTENT_CACHE))
		return (NULL);

	arena->chunk_hooks = chunk_hooks_default;

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock, "arena_bin",
		    WITNESS_RANK_ARENA_BIN))
			return (NULL);
		bin->runcur = NULL;
		arena_run_heap_new(&bin->runs);
		if (config_stats)
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
	}

	for (i = 0; i < NPSIZES; i++)
		arena_run_heap_new(&arena->runs_avail[i]);

	return (arena);
}

void
arena_boot(void)
{
	unsigned i;

	arena_lg_dirty_mult_default_set(opt_lg_dirty_mult);
	arena_decay_time_default_set(opt_decay_time);

	/*
	 * Compute the header size such that it is large enough to contain the
	 * page map.  The page map is biased to omit entries for the header
	 * itself, so some iteration is necessary to compute the map bias.
	 *
	 * 1) Compute safe header_size and map_bias values that include enough
	 *    space for an unbiased page map.
	 * 2) Refine map_bias based on (1) to omit the header pages in the page
	 *    map.  The resulting map_bias may be one too small.
	 * 3) Refine map_bias based on (2).  The result will be >= the result
	 *    from (2), and will always be correct.
	 */
	map_bias = 0;
	for (i = 0; i < 3; i++) {
		size_t header_size = offsetof(arena_chunk_t, map_bits) +
		    ((sizeof(arena_chunk_map_bits_t) +
		    sizeof(arena_chunk_map_misc_t)) * (chunk_npages-map_bias));
		map_bias = (header_size + PAGE_MASK) >> LG_PAGE;
	}
	assert(map_bias > 0);

	map_misc_offset = offsetof(arena_chunk_t, map_bits) +
	    sizeof(arena_chunk_map_bits_t) * (chunk_npages-map_bias);

	arena_maxrun = chunksize - (map_bias << LG_PAGE);
	assert(arena_maxrun > 0);
	large_maxclass = index2size(size2index(chunksize)-1);
	if (large_maxclass > arena_maxrun) {
		/*
		 * For small chunk sizes it's possible for there to be fewer
		 * non-header pages available than are necessary to serve the
		 * size classes just below chunksize.
		 */
		large_maxclass = arena_maxrun;
	}
	assert(large_maxclass > 0);
	nlclasses = size2index(large_maxclass) - size2index(SMALL_MAXCLASS);
	nhclasses = NSIZES - nlclasses - NBINS;
}

void
arena_prefork0(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_prefork(tsdn, &arena->lock);
}

void
arena_prefork1(tsdn_t *tsdn, arena_t *arena)
{

	malloc_mutex_prefork(tsdn, &arena->chunks_mtx);
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
	malloc_mutex_prefork(tsdn, &arena->huge_mtx);
}

void
arena_postfork_parent(tsdn_t *tsdn, arena_t *arena)
{
	unsigned i;

	malloc_mutex_postfork_parent(tsdn, &arena->huge_mtx);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_parent(tsdn, &arena->bins[i].lock);
	malloc_mutex_postfork_parent(tsdn, &arena->extent_cache_mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->chunks_mtx);
	malloc_mutex_postfork_parent(tsdn, &arena->lock);
}

void
arena_postfork_child(tsdn_t *tsdn, arena_t *arena)
{
	unsigned i;

	malloc_mutex_postfork_child(tsdn, &arena->huge_mtx);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_child(tsdn, &arena->bins[i].lock);
	malloc_mutex_postfork_child(tsdn, &arena->extent_cache_mtx);
	malloc_mutex_postfork_child(tsdn, &arena->chunks_mtx);
	malloc_mutex_postfork_child(tsdn, &arena->lock);
}
