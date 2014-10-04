#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
arena_bin_info_t	arena_bin_info[NBINS];

JEMALLOC_ALIGNED(CACHELINE)
const uint32_t	small_bin2size_tab[NBINS] = {
#define	B2S_bin_yes(size) \
	size,
#define	B2S_bin_no(size)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup) \
	B2S_bin_##bin((ZU(1)<<lg_grp) + (ZU(ndelta)<<lg_delta))
	SIZE_CLASSES
#undef B2S_bin_yes
#undef B2S_bin_no
#undef SC
};

JEMALLOC_ALIGNED(CACHELINE)
const uint8_t	small_size2bin_tab[] = {
#define	S2B_3(i)	i,
#define	S2B_4(i)	S2B_3(i) S2B_3(i)
#define	S2B_5(i)	S2B_4(i) S2B_4(i)
#define	S2B_6(i)	S2B_5(i) S2B_5(i)
#define	S2B_7(i)	S2B_6(i) S2B_6(i)
#define	S2B_8(i)	S2B_7(i) S2B_7(i)
#define	S2B_9(i)	S2B_8(i) S2B_8(i)
#define	S2B_no(i)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup) \
	S2B_##lg_delta_lookup(index)
	SIZE_CLASSES
#undef S2B_3
#undef S2B_4
#undef S2B_5
#undef S2B_6
#undef S2B_7
#undef S2B_8
#undef S2B_9
#undef S2B_no
#undef SC
};

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	arena_purge(arena_t *arena, bool all);
static void	arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty,
    bool cleaned);
static void	arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
static void	arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);

/******************************************************************************/

JEMALLOC_INLINE_C size_t
arena_miscelm_to_bits(arena_chunk_map_misc_t *miscelm)
{
	arena_chunk_t *chunk = CHUNK_ADDR2BASE(miscelm);
	size_t pageind = arena_miscelm_to_pageind(miscelm);

	return arena_mapbits_get(chunk, pageind);
}

static inline int
arena_run_comp(arena_chunk_map_misc_t *a, arena_chunk_map_misc_t *b)
{
	uintptr_t a_miscelm = (uintptr_t)a;
	uintptr_t b_miscelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_miscelm > b_miscelm) - (a_miscelm < b_miscelm));
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_run_tree_, arena_run_tree_t, arena_chunk_map_misc_t,
    rb_link, arena_run_comp)

static inline int
arena_avail_comp(arena_chunk_map_misc_t *a, arena_chunk_map_misc_t *b)
{
	int ret;
	size_t a_size;
	size_t b_size = arena_miscelm_to_bits(b) & ~PAGE_MASK;
	uintptr_t a_miscelm = (uintptr_t)a;
	uintptr_t b_miscelm = (uintptr_t)b;

	if (a_miscelm & CHUNK_MAP_KEY)
		a_size = a_miscelm & ~PAGE_MASK;
	else
		a_size = arena_miscelm_to_bits(a) & ~PAGE_MASK;

	ret = (a_size > b_size) - (a_size < b_size);
	if (ret == 0) {
		if (!(a_miscelm & CHUNK_MAP_KEY))
			ret = (a_miscelm > b_miscelm) - (a_miscelm < b_miscelm);
		else {
			/*
			 * Treat keys as if they are lower than anything else.
			 */
			ret = -1;
		}
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(static UNUSED, arena_avail_tree_, arena_avail_tree_t,
    arena_chunk_map_misc_t, rb_link, arena_avail_comp)

static void
arena_avail_insert(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_avail_tree_insert(&arena->runs_avail, arena_miscelm_get(chunk,
	    pageind));
}

static void
arena_avail_remove(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{

	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	arena_avail_tree_remove(&arena->runs_avail, arena_miscelm_get(chunk,
	    pageind));
}

static void
arena_dirty_insert(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);
	ql_elm_new(miscelm, dr_link);
	ql_tail_insert(&arena->runs_dirty, miscelm, dr_link);
	arena->ndirty += npages;
}

static void
arena_dirty_remove(arena_t *arena, arena_chunk_t *chunk, size_t pageind,
    size_t npages)
{
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	assert(npages == (arena_mapbits_unallocated_size_get(chunk, pageind) >>
	    LG_PAGE));
	assert(arena_mapbits_dirty_get(chunk, pageind) == CHUNK_MAP_DIRTY);
	assert(arena_mapbits_dirty_get(chunk, pageind+npages-1) ==
	    CHUNK_MAP_DIRTY);
	ql_remove(&arena->runs_dirty, miscelm, dr_link);
	arena->ndirty -= npages;
}

static inline void *
arena_run_reg_alloc(arena_run_t *run, arena_bin_info_t *bin_info)
{
	void *ret;
	unsigned regind;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(run->nfree > 0);
	assert(!bitmap_full(run->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(run->bitmap, &bin_info->bitmap_info);
	miscelm = arena_run_to_miscelm(run);
	rpages = arena_miscelm_to_rpages(miscelm);
	ret = (void *)((uintptr_t)rpages + (uintptr_t)bin_info->reg0_offset +
	    (uintptr_t)(bin_info->reg_interval * regind));
	run->nfree--;
	if (regind == run->nextind)
		run->nextind++;
	assert(regind < run->nextind);
	return (ret);
}

static inline void
arena_run_reg_dalloc(arena_run_t *run, void *ptr)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t mapbits = arena_mapbits_get(chunk, pageind);
	size_t binind = arena_ptr_small_binind_get(ptr, mapbits);
	arena_bin_info_t *bin_info = &arena_bin_info[binind];
	unsigned regind = arena_run_regind(run, bin_info, ptr);

	assert(run->nfree < bin_info->nregs);
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr -
	    ((uintptr_t)arena_miscelm_to_rpages(arena_run_to_miscelm(run)) +
	    (uintptr_t)bin_info->reg0_offset)) %
	    (uintptr_t)bin_info->reg_interval == 0);
	assert((uintptr_t)ptr >=
	    (uintptr_t)arena_miscelm_to_rpages(arena_run_to_miscelm(run)) +
	    (uintptr_t)bin_info->reg0_offset);
	/* Freeing an unallocated pointer can cause assertion failure. */
	assert(bitmap_get(run->bitmap, &bin_info->bitmap_info, regind));

	bitmap_unset(run->bitmap, &bin_info->bitmap_info, regind);
	run->nfree++;
}

static inline void
arena_run_zero(arena_chunk_t *chunk, size_t run_ind, size_t npages)
{

	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
	    (run_ind << LG_PAGE)), (npages << LG_PAGE));
	memset((void *)((uintptr_t)chunk + (run_ind << LG_PAGE)), 0,
	    (npages << LG_PAGE));
}

static inline void
arena_run_page_mark_zeroed(arena_chunk_t *chunk, size_t run_ind)
{

	JEMALLOC_VALGRIND_MAKE_MEM_DEFINED((void *)((uintptr_t)chunk + (run_ind
	    << LG_PAGE)), PAGE);
}

static inline void
arena_run_page_validate_zeroed(arena_chunk_t *chunk, size_t run_ind)
{
	size_t i;
	UNUSED size_t *p = (size_t *)((uintptr_t)chunk + (run_ind << LG_PAGE));

	arena_run_page_mark_zeroed(chunk, run_ind);
	for (i = 0; i < PAGE / sizeof(size_t); i++)
		assert(p[i] == 0);
}

static void
arena_cactive_update(arena_t *arena, size_t add_pages, size_t sub_pages)
{

	if (config_stats) {
		ssize_t cactive_diff = CHUNK_CEILING((arena->nactive + add_pages
		    - sub_pages) << LG_PAGE) - CHUNK_CEILING(arena->nactive <<
		    LG_PAGE);
		if (cactive_diff != 0)
			stats_cactive_add(cactive_diff);
	}
}

static void
arena_run_split_remove(arena_t *arena, arena_chunk_t *chunk, size_t run_ind,
    size_t flag_dirty, size_t need_pages)
{
	size_t total_pages, rem_pages;

	total_pages = arena_mapbits_unallocated_size_get(chunk, run_ind) >>
	    LG_PAGE;
	assert(arena_mapbits_dirty_get(chunk, run_ind+total_pages-1) ==
	    flag_dirty);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_remove(arena, chunk, run_ind, total_pages);
	if (flag_dirty != 0)
		arena_dirty_remove(arena, chunk, run_ind, total_pages);
	arena_cactive_update(arena, need_pages, 0);
	arena->nactive += need_pages;

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		if (flag_dirty != 0) {
			arena_mapbits_unallocated_set(chunk,
			    run_ind+need_pages, (rem_pages << LG_PAGE),
			    flag_dirty);
			arena_mapbits_unallocated_set(chunk,
			    run_ind+total_pages-1, (rem_pages << LG_PAGE),
			    flag_dirty);
			arena_dirty_insert(arena, chunk, run_ind+need_pages,
			    rem_pages);
		} else {
			arena_mapbits_unallocated_set(chunk, run_ind+need_pages,
			    (rem_pages << LG_PAGE),
			    arena_mapbits_unzeroed_get(chunk,
			    run_ind+need_pages));
			arena_mapbits_unallocated_set(chunk,
			    run_ind+total_pages-1, (rem_pages << LG_PAGE),
			    arena_mapbits_unzeroed_get(chunk,
			    run_ind+total_pages-1));
		}
		arena_avail_insert(arena, chunk, run_ind+need_pages, rem_pages);
	}
}

static void
arena_run_split_large_helper(arena_t *arena, arena_run_t *run, size_t size,
    bool remove, bool zero)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, run_ind, need_pages, i;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	if (remove) {
		arena_run_split_remove(arena, chunk, run_ind, flag_dirty,
		    need_pages);
	}

	if (zero) {
		if (flag_dirty == 0) {
			/*
			 * The run is clean, so some pages may be zeroed (i.e.
			 * never before touched).
			 */
			for (i = 0; i < need_pages; i++) {
				if (arena_mapbits_unzeroed_get(chunk, run_ind+i)
				    != 0)
					arena_run_zero(chunk, run_ind+i, 1);
				else if (config_debug) {
					arena_run_page_validate_zeroed(chunk,
					    run_ind+i);
				} else {
					arena_run_page_mark_zeroed(chunk,
					    run_ind+i);
				}
			}
		} else {
			/* The run is dirty, so all pages must be zeroed. */
			arena_run_zero(chunk, run_ind, need_pages);
		}
	} else {
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
		    (run_ind << LG_PAGE)), (need_pages << LG_PAGE));
	}

	/*
	 * Set the last element first, in case the run only contains one page
	 * (i.e. both statements set the same element).
	 */
	arena_mapbits_large_set(chunk, run_ind+need_pages-1, 0, flag_dirty);
	arena_mapbits_large_set(chunk, run_ind, size, flag_dirty);
}

static void
arena_run_split_large(arena_t *arena, arena_run_t *run, size_t size, bool zero)
{

	arena_run_split_large_helper(arena, run, size, true, zero);
}

static void
arena_run_init_large(arena_t *arena, arena_run_t *run, size_t size, bool zero)
{

	arena_run_split_large_helper(arena, run, size, false, zero);
}

static void
arena_run_split_small(arena_t *arena, arena_run_t *run, size_t size,
    size_t binind)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t flag_dirty, run_ind, need_pages, i;

	assert(binind != BININD_INVALID);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	flag_dirty = arena_mapbits_dirty_get(chunk, run_ind);
	need_pages = (size >> LG_PAGE);
	assert(need_pages > 0);

	arena_run_split_remove(arena, chunk, run_ind, flag_dirty, need_pages);

	/*
	 * Propagate the dirty and unzeroed flags to the allocated small run,
	 * so that arena_dalloc_bin_run() has the ability to conditionally trim
	 * clean pages.
	 */
	arena_mapbits_small_set(chunk, run_ind, 0, binind, flag_dirty);
	if (config_debug && flag_dirty == 0 && arena_mapbits_unzeroed_get(chunk,
	    run_ind) == 0)
		arena_run_page_validate_zeroed(chunk, run_ind);
	for (i = 1; i < need_pages - 1; i++) {
		arena_mapbits_small_set(chunk, run_ind+i, i, binind, 0);
		if (config_debug && flag_dirty == 0 &&
		    arena_mapbits_unzeroed_get(chunk, run_ind+i) == 0)
			arena_run_page_validate_zeroed(chunk, run_ind+i);
	}
	arena_mapbits_small_set(chunk, run_ind+need_pages-1, need_pages-1,
	    binind, flag_dirty);
	if (config_debug && flag_dirty == 0 && arena_mapbits_unzeroed_get(chunk,
	    run_ind+need_pages-1) == 0)
		arena_run_page_validate_zeroed(chunk, run_ind+need_pages-1);
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED((void *)((uintptr_t)chunk +
	    (run_ind << LG_PAGE)), (need_pages << LG_PAGE));
}

static arena_chunk_t *
arena_chunk_init_spare(arena_t *arena)
{
	arena_chunk_t *chunk;

	assert(arena->spare != NULL);

	chunk = arena->spare;
	arena->spare = NULL;

	assert(arena_mapbits_allocated_get(chunk, map_bias) == 0);
	assert(arena_mapbits_allocated_get(chunk, chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get(chunk, map_bias) ==
	    arena_maxclass);
	assert(arena_mapbits_unallocated_size_get(chunk, chunk_npages-1) ==
	    arena_maxclass);
	assert(arena_mapbits_dirty_get(chunk, map_bias) ==
	    arena_mapbits_dirty_get(chunk, chunk_npages-1));

	return (chunk);
}

static arena_chunk_t *
arena_chunk_alloc_internal(arena_t *arena, size_t size, size_t alignment,
    bool *zero)
{
	arena_chunk_t *chunk;
	chunk_alloc_t *chunk_alloc;
	chunk_dalloc_t *chunk_dalloc;

	chunk_alloc = arena->chunk_alloc;
	chunk_dalloc = arena->chunk_dalloc;
	malloc_mutex_unlock(&arena->lock);
	chunk = (arena_chunk_t *)chunk_alloc_arena(chunk_alloc, chunk_dalloc,
	    arena->ind, NULL, size, alignment, zero);
	malloc_mutex_lock(&arena->lock);
	if (config_stats && chunk != NULL)
		arena->stats.mapped += chunksize;

	return (chunk);
}

void *
arena_chunk_alloc_huge(arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero)
{
	void *ret;
	chunk_alloc_t *chunk_alloc;
	chunk_dalloc_t *chunk_dalloc;

	malloc_mutex_lock(&arena->lock);
	chunk_alloc = arena->chunk_alloc;
	chunk_dalloc = arena->chunk_dalloc;
	if (config_stats) {
		/* Optimistically update stats prior to unlocking. */
		arena->stats.mapped += size;
		arena->stats.allocated_huge += size;
		arena->stats.nmalloc_huge++;
		arena->stats.nrequests_huge++;
	}
	arena->nactive += (size >> LG_PAGE);
	malloc_mutex_unlock(&arena->lock);

	ret = chunk_alloc_arena(chunk_alloc, chunk_dalloc, arena->ind,
	    new_addr, size, alignment, zero);
	if (config_stats) {
		if (ret != NULL)
			stats_cactive_add(size);
		else {
			/* Revert optimistic stats updates. */
			malloc_mutex_lock(&arena->lock);
			arena->stats.mapped -= size;
			arena->stats.allocated_huge -= size;
			arena->stats.nmalloc_huge--;
			malloc_mutex_unlock(&arena->lock);
		}
	}

	return (ret);
}

static arena_chunk_t *
arena_chunk_init_hard(arena_t *arena)
{
	arena_chunk_t *chunk;
	bool zero;
	size_t unzeroed, i;

	assert(arena->spare == NULL);

	zero = false;
	chunk = arena_chunk_alloc_internal(arena, chunksize, chunksize, &zero);
	if (chunk == NULL)
		return (NULL);

	chunk->arena = arena;

	/*
	 * Initialize the map to contain one maximal free untouched run.  Mark
	 * the pages as zeroed iff chunk_alloc() returned a zeroed chunk.
	 */
	unzeroed = zero ? 0 : CHUNK_MAP_UNZEROED;
	arena_mapbits_unallocated_set(chunk, map_bias, arena_maxclass,
	    unzeroed);
	/*
	 * There is no need to initialize the internal page map entries unless
	 * the chunk is not zeroed.
	 */
	if (!zero) {
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(
		    (void *)arena_bitselm_get(chunk, map_bias+1),
		    (size_t)((uintptr_t) arena_bitselm_get(chunk,
		    chunk_npages-1) - (uintptr_t)arena_bitselm_get(chunk,
		    map_bias+1)));
		for (i = map_bias+1; i < chunk_npages-1; i++)
			arena_mapbits_unzeroed_set(chunk, i, unzeroed);
	} else {
		JEMALLOC_VALGRIND_MAKE_MEM_DEFINED((void
		    *)arena_bitselm_get(chunk, map_bias+1), (size_t)((uintptr_t)
		    arena_bitselm_get(chunk, chunk_npages-1) -
		    (uintptr_t)arena_bitselm_get(chunk, map_bias+1)));
		if (config_debug) {
			for (i = map_bias+1; i < chunk_npages-1; i++) {
				assert(arena_mapbits_unzeroed_get(chunk, i) ==
				    unzeroed);
			}
		}
	}
	arena_mapbits_unallocated_set(chunk, chunk_npages-1, arena_maxclass,
	    unzeroed);

	return (chunk);
}

static arena_chunk_t *
arena_chunk_alloc(arena_t *arena)
{
	arena_chunk_t *chunk;

	if (arena->spare != NULL)
		chunk = arena_chunk_init_spare(arena);
	else {
		chunk = arena_chunk_init_hard(arena);
		if (chunk == NULL)
			return (NULL);
	}

	/* Insert the run into the runs_avail tree. */
	arena_avail_insert(arena, chunk, map_bias, chunk_npages-map_bias);

	return (chunk);
}

static void
arena_chunk_dalloc_internal(arena_t *arena, arena_chunk_t *chunk)
{
	chunk_dalloc_t *chunk_dalloc;

	chunk_dalloc = arena->chunk_dalloc;
	malloc_mutex_unlock(&arena->lock);
	chunk_dalloc((void *)chunk, chunksize, arena->ind);
	malloc_mutex_lock(&arena->lock);
	if (config_stats)
		arena->stats.mapped -= chunksize;
}

void
arena_chunk_dalloc_huge(arena_t *arena, void *chunk, size_t size)
{
	chunk_dalloc_t *chunk_dalloc;

	malloc_mutex_lock(&arena->lock);
	chunk_dalloc = arena->chunk_dalloc;
	if (config_stats) {
		arena->stats.mapped -= size;
		arena->stats.allocated_huge -= size;
		arena->stats.ndalloc_huge++;
		stats_cactive_sub(size);
	}
	arena->nactive -= (size >> LG_PAGE);
	malloc_mutex_unlock(&arena->lock);
	chunk_dalloc(chunk, size, arena->ind);
}

static void
arena_chunk_dalloc(arena_t *arena, arena_chunk_t *chunk)
{

	assert(arena_mapbits_allocated_get(chunk, map_bias) == 0);
	assert(arena_mapbits_allocated_get(chunk, chunk_npages-1) == 0);
	assert(arena_mapbits_unallocated_size_get(chunk, map_bias) ==
	    arena_maxclass);
	assert(arena_mapbits_unallocated_size_get(chunk, chunk_npages-1) ==
	    arena_maxclass);
	assert(arena_mapbits_dirty_get(chunk, map_bias) ==
	    arena_mapbits_dirty_get(chunk, chunk_npages-1));

	/*
	 * Remove run from the runs_avail tree, so that the arena does not use
	 * it.
	 */
	arena_avail_remove(arena, chunk, map_bias, chunk_npages-map_bias);

	if (arena->spare != NULL) {
		arena_chunk_t *spare = arena->spare;

		arena->spare = chunk;
		if (arena_mapbits_dirty_get(spare, map_bias) != 0) {
			arena_dirty_remove(arena, spare, map_bias,
			    chunk_npages-map_bias);
		}
		arena_chunk_dalloc_internal(arena, spare);
	} else
		arena->spare = chunk;
}

static arena_run_t *
arena_run_alloc_large_helper(arena_t *arena, size_t size, bool zero)
{
	arena_chunk_map_misc_t *miscelm;
	arena_chunk_map_misc_t *key;

	key = (arena_chunk_map_misc_t *)(size | CHUNK_MAP_KEY);
	miscelm = arena_avail_tree_nsearch(&arena->runs_avail, key);
	if (miscelm != NULL) {
		arena_run_t *run = &miscelm->run;
		arena_run_split_large(arena, &miscelm->run, size, zero);
		return (run);
	}

	return (NULL);
}

static arena_run_t *
arena_run_alloc_large(arena_t *arena, size_t size, bool zero)
{
	arena_chunk_t *chunk;
	arena_run_t *run;

	assert(size <= arena_maxclass);
	assert((size & PAGE_MASK) == 0);

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_large_helper(arena, size, zero);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = &arena_miscelm_get(chunk, map_bias)->run;
		arena_run_split_large(arena, run, size, zero);
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_large_helper(arena, size, zero));
}

static arena_run_t *
arena_run_alloc_small_helper(arena_t *arena, size_t size, size_t binind)
{
	arena_run_t *run;
	arena_chunk_map_misc_t *miscelm;
	arena_chunk_map_misc_t *key;

	key = (arena_chunk_map_misc_t *)(size | CHUNK_MAP_KEY);
	miscelm = arena_avail_tree_nsearch(&arena->runs_avail, key);
	if (miscelm != NULL) {
		run = &miscelm->run;
		arena_run_split_small(arena, run, size, binind);
		return (run);
	}

	return (NULL);
}

static arena_run_t *
arena_run_alloc_small(arena_t *arena, size_t size, size_t binind)
{
	arena_chunk_t *chunk;
	arena_run_t *run;

	assert(size <= arena_maxclass);
	assert((size & PAGE_MASK) == 0);
	assert(binind != BININD_INVALID);

	/* Search the arena's chunks for the lowest best fit. */
	run = arena_run_alloc_small_helper(arena, size, binind);
	if (run != NULL)
		return (run);

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = &arena_miscelm_get(chunk, map_bias)->run;
		arena_run_split_small(arena, run, size, binind);
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	return (arena_run_alloc_small_helper(arena, size, binind));
}

static inline void
arena_maybe_purge(arena_t *arena)
{
	size_t threshold;

	/* Don't purge if the option is disabled. */
	if (opt_lg_dirty_mult < 0)
		return;
	threshold = (arena->nactive >> opt_lg_dirty_mult);
	/*
	 * Don't purge unless the number of purgeable pages exceeds the
	 * threshold.
	 */
	if (arena->ndirty <= threshold)
		return;

	arena_purge(arena, false);
}

static size_t
arena_dirty_count(arena_t *arena)
{
	size_t ndirty = 0;
	arena_chunk_map_misc_t *miscelm;
	arena_chunk_t *chunk;
	size_t pageind, npages;

	ql_foreach(miscelm, &arena->runs_dirty, dr_link) {
		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
		pageind = arena_miscelm_to_pageind(miscelm);
		assert(arena_mapbits_allocated_get(chunk, pageind) == 0);
		assert(arena_mapbits_large_get(chunk, pageind) == 0);
		assert(arena_mapbits_dirty_get(chunk, pageind) != 0);
		npages = arena_mapbits_unallocated_size_get(chunk, pageind) >>
		    LG_PAGE;
		ndirty += npages;
	}

       return (ndirty);
}

static size_t
arena_compute_npurge(arena_t *arena, bool all)
{
	size_t npurge;

	/*
	 * Compute the minimum number of pages that this thread should try to
	 * purge.
	 */
	if (!all) {
		size_t threshold = (arena->nactive >> opt_lg_dirty_mult);

		npurge = arena->ndirty - threshold;
	} else
		npurge = arena->ndirty;

	return (npurge);
}

static size_t
arena_stash_dirty(arena_t *arena, bool all, size_t npurge,
    arena_chunk_miscelms_t *miscelms)
{
	arena_chunk_map_misc_t *miscelm;
	size_t nstashed = 0;

	/* Add at least npurge pages to purge_list. */
	for (miscelm = ql_first(&arena->runs_dirty); miscelm != NULL;
	    miscelm = ql_first(&arena->runs_dirty)) {
		arena_chunk_t *chunk =
		    (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
		size_t pageind = arena_miscelm_to_pageind(miscelm);
		size_t run_size = arena_mapbits_unallocated_size_get(chunk,
		    pageind);
		size_t npages = run_size >> LG_PAGE;
		arena_run_t *run = &miscelm->run;

		assert(pageind + npages <= chunk_npages);
		assert(arena_mapbits_dirty_get(chunk, pageind) ==
		    arena_mapbits_dirty_get(chunk, pageind+npages-1));

		/*
		 * If purging the spare chunk's run, make it available prior to
		 * allocation.
		 */
		if (chunk == arena->spare)
			arena_chunk_alloc(arena);

		/* Temporarily allocate the free dirty run. */
		arena_run_split_large(arena, run, run_size, false);
		/* Append to purge_list for later processing. */
		ql_elm_new(miscelm, dr_link);
		ql_tail_insert(miscelms, miscelm, dr_link);

		nstashed += npages;

		if (!all && nstashed >= npurge)
			break;
	}

	return (nstashed);
}

static size_t
arena_purge_stashed(arena_t *arena, arena_chunk_miscelms_t *miscelms)
{
	size_t npurged, nmadvise;
	arena_chunk_map_misc_t *miscelm;

	if (config_stats)
		nmadvise = 0;
	npurged = 0;

	malloc_mutex_unlock(&arena->lock);

	ql_foreach(miscelm, miscelms, dr_link) {
		arena_chunk_t *chunk;
		size_t pageind, run_size, npages, flag_unzeroed, i;
		bool unzeroed;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
		pageind = arena_miscelm_to_pageind(miscelm);
		run_size = arena_mapbits_large_size_get(chunk, pageind);
		npages = run_size >> LG_PAGE;

		assert(pageind + npages <= chunk_npages);
		unzeroed = pages_purge((void *)((uintptr_t)chunk + (pageind <<
		    LG_PAGE)), run_size);
		flag_unzeroed = unzeroed ? CHUNK_MAP_UNZEROED : 0;

		/*
		 * Set the unzeroed flag for all pages, now that pages_purge()
		 * has returned whether the pages were zeroed as a side effect
		 * of purging.  This chunk map modification is safe even though
		 * the arena mutex isn't currently owned by this thread,
		 * because the run is marked as allocated, thus protecting it
		 * from being modified by any other thread.  As long as these
		 * writes don't perturb the first and last elements'
		 * CHUNK_MAP_ALLOCATED bits, behavior is well defined.
		 */
		for (i = 0; i < npages; i++) {
			arena_mapbits_unzeroed_set(chunk, pageind+i,
			    flag_unzeroed);
		}

		npurged += npages;
		if (config_stats)
			nmadvise++;
	}

	malloc_mutex_lock(&arena->lock);

	if (config_stats) {
		arena->stats.nmadvise += nmadvise;
		arena->stats.purged += npurged;
	}

	return (npurged);
}

static void
arena_unstash_purged(arena_t *arena, arena_chunk_miscelms_t *miscelms)
{
	arena_chunk_map_misc_t *miscelm;

	/* Deallocate runs. */
	for (miscelm = ql_first(miscelms); miscelm != NULL;
	    miscelm = ql_first(miscelms)) {
		arena_run_t *run = &miscelm->run;
		ql_remove(miscelms, miscelm, dr_link);
		arena_run_dalloc(arena, run, false, true);
	}
}

void
arena_purge(arena_t *arena, bool all)
{
	size_t npurge, npurgeable, npurged;
	arena_chunk_miscelms_t purge_list;

	if (config_debug) {
		size_t ndirty = arena_dirty_count(arena);
		assert(ndirty == arena->ndirty);
	}
	assert((arena->nactive >> opt_lg_dirty_mult) < arena->ndirty || all);

	if (config_stats)
		arena->stats.npurge++;

	npurge = arena_compute_npurge(arena, all);
	ql_new(&purge_list);
	npurgeable = arena_stash_dirty(arena, all, npurge, &purge_list);
	assert(npurgeable >= npurge);
	npurged = arena_purge_stashed(arena, &purge_list);
	assert(npurged == npurgeable);
	arena_unstash_purged(arena, &purge_list);
}

void
arena_purge_all(arena_t *arena)
{

	malloc_mutex_lock(&arena->lock);
	arena_purge(arena, true);
	malloc_mutex_unlock(&arena->lock);
}

static void
arena_run_coalesce(arena_t *arena, arena_chunk_t *chunk, size_t *p_size,
    size_t *p_run_ind, size_t *p_run_pages, size_t flag_dirty)
{
	size_t size = *p_size;
	size_t run_ind = *p_run_ind;
	size_t run_pages = *p_run_pages;

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    arena_mapbits_allocated_get(chunk, run_ind+run_pages) == 0 &&
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages) == flag_dirty) {
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
		arena_avail_remove(arena, chunk, run_ind+run_pages, nrun_pages);

		/* If the successor is dirty, remove it from runs_dirty. */
		if (flag_dirty != 0) {
			arena_dirty_remove(arena, chunk, run_ind+run_pages,
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
	    flag_dirty) {
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
		arena_avail_remove(arena, chunk, run_ind, prun_pages);

		/* If the predecessor is dirty, remove it from runs_dirty. */
		if (flag_dirty != 0)
			arena_dirty_remove(arena, chunk, run_ind, prun_pages);

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

static void
arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty, bool cleaned)
{
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	size_t size, run_ind, run_pages, flag_dirty;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	assert(run_ind >= map_bias);
	assert(run_ind < chunk_npages);
	if (arena_mapbits_large_get(chunk, run_ind) != 0) {
		size = arena_mapbits_large_size_get(chunk, run_ind);
		assert(size == PAGE ||
		    arena_mapbits_large_size_get(chunk,
		    run_ind+(size>>LG_PAGE)-1) == 0);
	} else {
		size_t binind = arena_bin_index(arena, run->bin);
		arena_bin_info_t *bin_info = &arena_bin_info[binind];
		size = bin_info->run_size;
	}
	run_pages = (size >> LG_PAGE);
	arena_cactive_update(arena, 0, run_pages);
	arena->nactive -= run_pages;

	/*
	 * The run is dirty if the caller claims to have dirtied it, as well as
	 * if it was already dirty before being allocated and the caller
	 * doesn't claim to have cleaned it.
	 */
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	if (!cleaned && arena_mapbits_dirty_get(chunk, run_ind) != 0)
		dirty = true;
	flag_dirty = dirty ? CHUNK_MAP_DIRTY : 0;

	/* Mark pages as unallocated in the chunk map. */
	if (dirty) {
		arena_mapbits_unallocated_set(chunk, run_ind, size,
		    CHUNK_MAP_DIRTY);
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    CHUNK_MAP_DIRTY);
	} else {
		arena_mapbits_unallocated_set(chunk, run_ind, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind));
		arena_mapbits_unallocated_set(chunk, run_ind+run_pages-1, size,
		    arena_mapbits_unzeroed_get(chunk, run_ind+run_pages-1));
	}

	arena_run_coalesce(arena, chunk, &size, &run_ind, &run_pages, flag_dirty);

	/* Insert into runs_avail, now that coalescing is complete. */
	assert(arena_mapbits_unallocated_size_get(chunk, run_ind) ==
	    arena_mapbits_unallocated_size_get(chunk, run_ind+run_pages-1));
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+run_pages-1));
	arena_avail_insert(arena, chunk, run_ind, run_pages);

	if (dirty)
		arena_dirty_insert(arena, chunk, run_ind, run_pages);

	/* Deallocate chunk if it is now completely unused. */
	if (size == arena_maxclass) {
		assert(run_ind == map_bias);
		assert(run_pages == (arena_maxclass >> LG_PAGE));
		arena_chunk_dalloc(arena, chunk);
	}

	/*
	 * It is okay to do dirty page processing here even if the chunk was
	 * deallocated above, since in that case it is the spare.  Waiting
	 * until after possible chunk deallocation to do dirty processing
	 * allows for an old spare to be fully deallocated, thus decreasing the
	 * chances of spuriously crossing the dirty page purging threshold.
	 */
	if (dirty)
		arena_maybe_purge(arena);
}

static void
arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	size_t pageind = arena_miscelm_to_pageind(miscelm);
	size_t head_npages = (oldsize - newsize) >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty);
	arena_mapbits_large_set(chunk, pageind, oldsize-newsize, flag_dirty);

	if (config_debug) {
		UNUSED size_t tail_npages = newsize >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, newsize,
	    flag_dirty);

	arena_run_dalloc(arena, run, false, false);
}

static void
arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize, bool dirty)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	size_t pageind = arena_miscelm_to_pageind(miscelm);
	size_t head_npages = newsize >> LG_PAGE;
	size_t flag_dirty = arena_mapbits_dirty_get(chunk, pageind);
	arena_chunk_map_misc_t *tail_miscelm;
	arena_run_t *tail_run;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.  Set the last element of each
	 * run first, in case of single-page runs.
	 */
	assert(arena_mapbits_large_size_get(chunk, pageind) == oldsize);
	arena_mapbits_large_set(chunk, pageind+head_npages-1, 0, flag_dirty);
	arena_mapbits_large_set(chunk, pageind, newsize, flag_dirty);

	if (config_debug) {
		UNUSED size_t tail_npages = (oldsize - newsize) >> LG_PAGE;
		assert(arena_mapbits_large_size_get(chunk,
		    pageind+head_npages+tail_npages-1) == 0);
		assert(arena_mapbits_dirty_get(chunk,
		    pageind+head_npages+tail_npages-1) == flag_dirty);
	}
	arena_mapbits_large_set(chunk, pageind+head_npages, oldsize-newsize,
	    flag_dirty);

	tail_miscelm = arena_miscelm_get(chunk, pageind + head_npages);
	tail_run = &tail_miscelm->run;
	arena_run_dalloc(arena, tail_run, dirty, false);
}

static arena_run_t *
arena_bin_runs_first(arena_bin_t *bin)
{
	arena_chunk_map_misc_t *miscelm = arena_run_tree_first(&bin->runs);
	if (miscelm != NULL)
		return (&miscelm->run);

	return (NULL);
}

static void
arena_bin_runs_insert(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);

	assert(arena_run_tree_search(&bin->runs, miscelm) == NULL);

	arena_run_tree_insert(&bin->runs, miscelm);
}

static void
arena_bin_runs_remove(arena_bin_t *bin, arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);

	assert(arena_run_tree_search(&bin->runs, miscelm) != NULL);

	arena_run_tree_remove(&bin->runs, miscelm);
}

static arena_run_t *
arena_bin_nonfull_run_tryget(arena_bin_t *bin)
{
	arena_run_t *run = arena_bin_runs_first(bin);
	if (run != NULL) {
		arena_bin_runs_remove(bin, run);
		if (config_stats)
			bin->stats.reruns++;
	}
	return (run);
}

static arena_run_t *
arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin)
{
	arena_run_t *run;
	size_t binind;
	arena_bin_info_t *bin_info;

	/* Look for a usable run. */
	run = arena_bin_nonfull_run_tryget(bin);
	if (run != NULL)
		return (run);
	/* No existing runs have any space available. */

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];

	/* Allocate a new run. */
	malloc_mutex_unlock(&bin->lock);
	/******************************/
	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc_small(arena, bin_info->run_size, binind);
	if (run != NULL) {
		/* Initialize run internals. */
		run->bin = bin;
		run->nextind = 0;
		run->nfree = bin_info->nregs;
		bitmap_init(run->bitmap, &bin_info->bitmap_info);
	}
	malloc_mutex_unlock(&arena->lock);
	/********************************/
	malloc_mutex_lock(&bin->lock);
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
arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin)
{
	void *ret;
	size_t binind;
	arena_bin_info_t *bin_info;
	arena_run_t *run;

	binind = arena_bin_index(arena, bin);
	bin_info = &arena_bin_info[binind];
	bin->runcur = NULL;
	run = arena_bin_nonfull_run_get(arena, bin);
	if (bin->runcur != NULL && bin->runcur->nfree > 0) {
		/*
		 * Another thread updated runcur while this one ran without the
		 * bin lock in arena_bin_nonfull_run_get().
		 */
		assert(bin->runcur->nfree > 0);
		ret = arena_run_reg_alloc(bin->runcur, bin_info);
		if (run != NULL) {
			arena_chunk_t *chunk;

			/*
			 * arena_run_alloc_small() may have allocated run, or
			 * it may have pulled run from the bin's run tree.
			 * Therefore it is unsafe to make any assumptions about
			 * how run has previously been used, and
			 * arena_bin_lower_run() must be called, as if a region
			 * were just deallocated from the run.
			 */
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
			if (run->nfree == bin_info->nregs)
				arena_dalloc_bin_run(arena, chunk, run, bin);
			else
				arena_bin_lower_run(arena, chunk, run, bin);
		}
		return (ret);
	}

	if (run == NULL)
		return (NULL);

	bin->runcur = run;

	assert(bin->runcur->nfree > 0);

	return (arena_run_reg_alloc(bin->runcur, bin_info));
}

void
arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin, size_t binind,
    uint64_t prof_accumbytes)
{
	unsigned i, nfill;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;

	assert(tbin->ncached == 0);

	if (config_prof && arena_prof_accum(arena, prof_accumbytes))
		prof_idump();
	bin = &arena->bins[binind];
	malloc_mutex_lock(&bin->lock);
	for (i = 0, nfill = (tcache_bin_info[binind].ncached_max >>
	    tbin->lg_fill_div); i < nfill; i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_run_reg_alloc(run, &arena_bin_info[binind]);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL) {
			/*
			 * OOM.  tbin->avail isn't yet filled down to its first
			 * element, so the successful allocations (if any) must
			 * be moved to the base of tbin->avail before bailing
			 * out.
			 */
			if (i > 0) {
				memmove(tbin->avail, &tbin->avail[nfill - i],
				    i * sizeof(void *));
			}
			break;
		}
		if (config_fill && unlikely(opt_junk)) {
			arena_alloc_junk_small(ptr, &arena_bin_info[binind],
			    true);
		}
		/* Insert such that low regions get used first. */
		tbin->avail[nfill - 1 - i] = ptr;
	}
	if (config_stats) {
		bin->stats.allocated += i * arena_bin_info[binind].reg_size;
		bin->stats.nmalloc += i;
		bin->stats.nrequests += tbin->tstats.nrequests;
		bin->stats.nfills++;
		tbin->tstats.nrequests = 0;
	}
	malloc_mutex_unlock(&bin->lock);
	tbin->ncached = i;
}

void
arena_alloc_junk_small(void *ptr, arena_bin_info_t *bin_info, bool zero)
{

	if (zero) {
		size_t redzone_size = bin_info->redzone_size;
		memset((void *)((uintptr_t)ptr - redzone_size), 0xa5,
		    redzone_size);
		memset((void *)((uintptr_t)ptr + bin_info->reg_size), 0xa5,
		    redzone_size);
	} else {
		memset((void *)((uintptr_t)ptr - bin_info->redzone_size), 0xa5,
		    bin_info->reg_interval);
	}
}

#ifdef JEMALLOC_JET
#undef arena_redzone_corruption
#define	arena_redzone_corruption JEMALLOC_N(arena_redzone_corruption_impl)
#endif
static void
arena_redzone_corruption(void *ptr, size_t usize, bool after,
    size_t offset, uint8_t byte)
{

	malloc_printf("<jemalloc>: Corrupt redzone %zu byte%s %s %p "
	    "(size %zu), byte=%#x\n", offset, (offset == 1) ? "" : "s",
	    after ? "after" : "before", ptr, usize, byte);
}
#ifdef JEMALLOC_JET
#undef arena_redzone_corruption
#define	arena_redzone_corruption JEMALLOC_N(arena_redzone_corruption)
arena_redzone_corruption_t *arena_redzone_corruption =
    JEMALLOC_N(arena_redzone_corruption_impl);
#endif

static void
arena_redzones_validate(void *ptr, arena_bin_info_t *bin_info, bool reset)
{
	size_t size = bin_info->reg_size;
	size_t redzone_size = bin_info->redzone_size;
	size_t i;
	bool error = false;

	for (i = 1; i <= redzone_size; i++) {
		uint8_t *byte = (uint8_t *)((uintptr_t)ptr - i);
		if (*byte != 0xa5) {
			error = true;
			arena_redzone_corruption(ptr, size, false, i, *byte);
			if (reset)
				*byte = 0xa5;
		}
	}
	for (i = 0; i < redzone_size; i++) {
		uint8_t *byte = (uint8_t *)((uintptr_t)ptr + size + i);
		if (*byte != 0xa5) {
			error = true;
			arena_redzone_corruption(ptr, size, true, i, *byte);
			if (reset)
				*byte = 0xa5;
		}
	}
	if (opt_abort && error)
		abort();
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small_impl)
#endif
void
arena_dalloc_junk_small(void *ptr, arena_bin_info_t *bin_info)
{
	size_t redzone_size = bin_info->redzone_size;

	arena_redzones_validate(ptr, bin_info, false);
	memset((void *)((uintptr_t)ptr - redzone_size), 0x5a,
	    bin_info->reg_interval);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_small
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small)
arena_dalloc_junk_small_t *arena_dalloc_junk_small =
    JEMALLOC_N(arena_dalloc_junk_small_impl);
#endif

void
arena_quarantine_junk_small(void *ptr, size_t usize)
{
	size_t binind;
	arena_bin_info_t *bin_info;
	cassert(config_fill);
	assert(opt_junk);
	assert(opt_quarantine);
	assert(usize <= SMALL_MAXCLASS);

	binind = small_size2bin(usize);
	bin_info = &arena_bin_info[binind];
	arena_redzones_validate(ptr, bin_info, true);
}

void *
arena_malloc_small(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	arena_run_t *run;
	size_t binind;

	binind = small_size2bin(size);
	assert(binind < NBINS);
	bin = &arena->bins[binind];
	size = small_bin2size(binind);

	malloc_mutex_lock(&bin->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_run_reg_alloc(run, &arena_bin_info[binind]);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&bin->lock);
		return (NULL);
	}

	if (config_stats) {
		bin->stats.allocated += size;
		bin->stats.nmalloc++;
		bin->stats.nrequests++;
	}
	malloc_mutex_unlock(&bin->lock);
	if (config_prof && !isthreaded && arena_prof_accum(arena, size))
		prof_idump();

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk)) {
				arena_alloc_junk_small(ret,
				    &arena_bin_info[binind], false);
			} else if (unlikely(opt_zero))
				memset(ret, 0, size);
		}
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
	} else {
		if (config_fill && unlikely(opt_junk)) {
			arena_alloc_junk_small(ret, &arena_bin_info[binind],
			    true);
		}
		JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, size);
		memset(ret, 0, size);
	}

	return (ret);
}

void *
arena_malloc_large(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_run_t *run;
	arena_chunk_map_misc_t *miscelm;
	UNUSED bool idump;

	/* Large allocation. */
	size = PAGE_CEILING(size);
	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc_large(arena, size, zero);
	if (run == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	miscelm = arena_run_to_miscelm(run);
	ret = arena_miscelm_to_rpages(miscelm);
	if (config_stats) {
		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	if (config_prof)
		idump = arena_prof_accum_locked(arena, size);
	malloc_mutex_unlock(&arena->lock);
	if (config_prof && idump)
		prof_idump();

	if (!zero) {
		if (config_fill) {
			if (unlikely(opt_junk))
				memset(ret, 0xa5, size);
			else if (unlikely(opt_zero))
				memset(ret, 0, size);
		}
	}

	return (ret);
}

/* Only handles large allocations that require more than page alignment. */
void *
arena_palloc(arena_t *arena, size_t size, size_t alignment, bool zero)
{
	void *ret;
	size_t alloc_size, leadsize, trailsize;
	arena_run_t *run;
	arena_chunk_t *chunk;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert((size & PAGE_MASK) == 0);

	alignment = PAGE_CEILING(alignment);
	alloc_size = size + alignment - PAGE;

	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc_large(arena, alloc_size, false);
	if (run == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	miscelm = arena_run_to_miscelm(run);
	rpages = arena_miscelm_to_rpages(miscelm);

	leadsize = ALIGNMENT_CEILING((uintptr_t)rpages, alignment) -
	    (uintptr_t)rpages;
	assert(alloc_size >= leadsize + size);
	trailsize = alloc_size - leadsize - size;
	if (leadsize != 0) {
		arena_chunk_map_misc_t *head_miscelm = miscelm;
		arena_run_t *head_run = run;

		miscelm = arena_miscelm_get(chunk,
		    arena_miscelm_to_pageind(head_miscelm) + (leadsize >>
		    LG_PAGE));
		run = &miscelm->run;

		arena_run_trim_head(arena, chunk, head_run, alloc_size,
		    alloc_size - leadsize);
	}
	if (trailsize != 0) {
		arena_run_trim_tail(arena, chunk, run, size + trailsize, size,
		    false);
	}
	arena_run_init_large(arena, run, size, zero);
	ret = arena_miscelm_to_rpages(miscelm);

	if (config_stats) {
		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);

	if (config_fill && !zero) {
		if (unlikely(opt_junk))
			memset(ret, 0xa5, size);
		else if (unlikely(opt_zero))
			memset(ret, 0, size);
	}
	return (ret);
}

void
arena_prof_promoted(const void *ptr, size_t size)
{
	arena_chunk_t *chunk;
	size_t pageind, binind;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);
	assert(isalloc(ptr, false) == PAGE);
	assert(isalloc(ptr, true) == PAGE);
	assert(size <= SMALL_MAXCLASS);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	binind = small_size2bin(size);
	assert(binind < NBINS);
	arena_mapbits_large_binind_set(chunk, pageind, binind);

	assert(isalloc(ptr, false) == PAGE);
	assert(isalloc(ptr, true) == size);
}

static void
arena_dissociate_bin_run(arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/* Dissociate run from bin. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else {
		size_t binind = arena_bin_index(chunk->arena, bin);
		arena_bin_info_t *bin_info = &arena_bin_info[binind];

		if (bin_info->nregs != 1) {
			/*
			 * This block's conditional is necessary because if the
			 * run only contains one region, then it never gets
			 * inserted into the non-full runs tree.
			 */
			arena_bin_runs_remove(bin, run);
		}
	}
}

static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{
	size_t binind;
	arena_bin_info_t *bin_info;
	size_t npages, run_ind, past;
	arena_chunk_map_misc_t *miscelm;
	void *rpages;

	assert(run != bin->runcur);
	assert(arena_run_tree_search(&bin->runs, arena_run_to_miscelm(run)) ==
	    NULL);

	binind = arena_bin_index(chunk->arena, run->bin);
	bin_info = &arena_bin_info[binind];

	malloc_mutex_unlock(&bin->lock);
	/******************************/
	npages = bin_info->run_size >> LG_PAGE;
	miscelm = arena_run_to_miscelm(run);
	run_ind = arena_miscelm_to_pageind(miscelm);
	rpages = arena_miscelm_to_rpages(miscelm);
	past = (size_t)(PAGE_CEILING((uintptr_t)rpages +
	    (uintptr_t)bin_info->reg0_offset + (uintptr_t)(run->nextind *
	    bin_info->reg_interval - bin_info->redzone_size) -
	    (uintptr_t)chunk) >> LG_PAGE);
	malloc_mutex_lock(&arena->lock);

	/*
	 * If the run was originally clean, and some pages were never touched,
	 * trim the clean pages before deallocating the dirty portion of the
	 * run.
	 */
	assert(arena_mapbits_dirty_get(chunk, run_ind) ==
	    arena_mapbits_dirty_get(chunk, run_ind+npages-1));
	if (arena_mapbits_dirty_get(chunk, run_ind) == 0 && past - run_ind <
	    npages) {
		/* Trim clean pages.  Convert to large run beforehand. */
		assert(npages > 0);
		if (past > run_ind) {
			arena_mapbits_large_set(chunk, run_ind,
			    bin_info->run_size, 0);
			arena_mapbits_large_set(chunk, run_ind+npages-1, 0, 0);
			arena_run_trim_tail(arena, chunk, run, (npages <<
			    LG_PAGE), ((past - run_ind) << LG_PAGE), false);
			arena_run_dalloc(arena, run, true, false);
		} else
			arena_run_dalloc(arena, run, false, false);
		/* npages = past - run_ind; */
	} else
		arena_run_dalloc(arena, run, true, false);
	malloc_mutex_unlock(&arena->lock);
	/****************************/
	malloc_mutex_lock(&bin->lock);
	if (config_stats)
		bin->stats.curruns--;
}

static void
arena_bin_lower_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{

	/*
	 * Make sure that if bin->runcur is non-NULL, it refers to the lowest
	 * non-full run.  It is okay to NULL runcur out rather than proactively
	 * keeping it pointing at the lowest non-full run.
	 */
	if ((uintptr_t)run < (uintptr_t)bin->runcur) {
		/* Switch runcur. */
		if (bin->runcur->nfree > 0)
			arena_bin_runs_insert(bin, bin->runcur);
		bin->runcur = run;
		if (config_stats)
			bin->stats.reruns++;
	} else
		arena_bin_runs_insert(bin, run);
}

void
arena_dalloc_bin_locked(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_bits_t *bitselm)
{
	size_t pageind, rpages_ind;
	arena_run_t *run;
	arena_bin_t *bin;
	arena_bin_info_t *bin_info;
	size_t size, binind;

	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get(chunk, rpages_ind)->run;
	bin = run->bin;
	binind = arena_ptr_small_binind_get(ptr, arena_mapbits_get(chunk,
	    pageind));
	bin_info = &arena_bin_info[binind];
	if (config_fill || config_stats)
		size = bin_info->reg_size;

	if (config_fill && unlikely(opt_junk))
		arena_dalloc_junk_small(ptr, bin_info);

	arena_run_reg_dalloc(run, ptr);
	if (run->nfree == bin_info->nregs) {
		arena_dissociate_bin_run(chunk, run, bin);
		arena_dalloc_bin_run(arena, chunk, run, bin);
	} else if (run->nfree == 1 && run != bin->runcur)
		arena_bin_lower_run(arena, chunk, run, bin);

	if (config_stats) {
		bin->stats.allocated -= size;
		bin->stats.ndalloc++;
	}
}

void
arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind, arena_chunk_map_bits_t *bitselm)
{
	arena_run_t *run;
	arena_bin_t *bin;
	size_t rpages_ind;

	rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
	run = &arena_miscelm_get(chunk, rpages_ind)->run;
	bin = run->bin;
	malloc_mutex_lock(&bin->lock);
	arena_dalloc_bin_locked(arena, chunk, ptr, bitselm);
	malloc_mutex_unlock(&bin->lock);
}

void
arena_dalloc_small(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind)
{
	arena_chunk_map_bits_t *bitselm;

	if (config_debug) {
		/* arena_ptr_small_binind_get() does extra sanity checking. */
		assert(arena_ptr_small_binind_get(ptr, arena_mapbits_get(chunk,
		    pageind)) != BININD_INVALID);
	}
	bitselm = arena_bitselm_get(chunk, pageind);
	arena_dalloc_bin(arena, chunk, ptr, pageind, bitselm);
}

#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(arena_dalloc_junk_large_impl)
#endif
static void
arena_dalloc_junk_large(void *ptr, size_t usize)
{

	if (config_fill && unlikely(opt_junk))
		memset(ptr, 0x5a, usize);
}
#ifdef JEMALLOC_JET
#undef arena_dalloc_junk_large
#define	arena_dalloc_junk_large JEMALLOC_N(arena_dalloc_junk_large)
arena_dalloc_junk_large_t *arena_dalloc_junk_large =
    JEMALLOC_N(arena_dalloc_junk_large_impl);
#endif

void
arena_dalloc_large_locked(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	arena_run_t *run = &miscelm->run;

	if (config_fill || config_stats) {
		size_t usize = arena_mapbits_large_size_get(chunk, pageind);

		arena_dalloc_junk_large(ptr, usize);
		if (config_stats) {
			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= usize;
			arena->stats.lstats[(usize >> LG_PAGE) - 1].ndalloc++;
			arena->stats.lstats[(usize >> LG_PAGE) - 1].curruns--;
		}
	}

	arena_run_dalloc(arena, run, true, false);
}

void
arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{

	malloc_mutex_lock(&arena->lock);
	arena_dalloc_large_locked(arena, chunk, ptr);
	malloc_mutex_unlock(&arena->lock);
}

static void
arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	arena_chunk_map_misc_t *miscelm = arena_miscelm_get(chunk, pageind);
	arena_run_t *run = &miscelm->run;

	assert(size < oldsize);

	/*
	 * Shrink the run, and make trailing pages available for other
	 * allocations.
	 */
	malloc_mutex_lock(&arena->lock);
	arena_run_trim_tail(arena, chunk, run, oldsize, size, true);
	if (config_stats) {
		arena->stats.ndalloc_large++;
		arena->stats.allocated_large -= oldsize;
		arena->stats.lstats[(oldsize >> LG_PAGE) - 1].ndalloc++;
		arena->stats.lstats[(oldsize >> LG_PAGE) - 1].curruns--;

		arena->stats.nmalloc_large++;
		arena->stats.nrequests_large++;
		arena->stats.allocated_large += size;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
		arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
	}
	malloc_mutex_unlock(&arena->lock);
}

static bool
arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t oldsize, size_t size, size_t extra, bool zero)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	size_t npages = oldsize >> LG_PAGE;
	size_t followsize;

	assert(oldsize == arena_mapbits_large_size_get(chunk, pageind));

	/* Try to extend the run. */
	assert(size + extra > oldsize);
	malloc_mutex_lock(&arena->lock);
	if (pageind + npages < chunk_npages &&
	    arena_mapbits_allocated_get(chunk, pageind+npages) == 0 &&
	    (followsize = arena_mapbits_unallocated_size_get(chunk,
	    pageind+npages)) >= size - oldsize) {
		/*
		 * The next run is available and sufficiently large.  Split the
		 * following run, then merge the first part with the existing
		 * allocation.
		 */
		size_t flag_dirty;
		size_t splitsize = (oldsize + followsize <= size + extra)
		    ? followsize : size + extra - oldsize;
		arena_run_t *run = &arena_miscelm_get(chunk,
		    pageind+npages)->run;
		arena_run_split_large(arena, run, splitsize, zero);

		size = oldsize + splitsize;
		npages = size >> LG_PAGE;

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
		arena_mapbits_large_set(chunk, pageind, size, flag_dirty);
		arena_mapbits_large_set(chunk, pageind+npages-1, 0, flag_dirty);

		if (config_stats) {
			arena->stats.ndalloc_large++;
			arena->stats.allocated_large -= oldsize;
			arena->stats.lstats[(oldsize >> LG_PAGE) - 1].ndalloc++;
			arena->stats.lstats[(oldsize >> LG_PAGE) - 1].curruns--;

			arena->stats.nmalloc_large++;
			arena->stats.nrequests_large++;
			arena->stats.allocated_large += size;
			arena->stats.lstats[(size >> LG_PAGE) - 1].nmalloc++;
			arena->stats.lstats[(size >> LG_PAGE) - 1].nrequests++;
			arena->stats.lstats[(size >> LG_PAGE) - 1].curruns++;
		}
		malloc_mutex_unlock(&arena->lock);
		return (false);
	}
	malloc_mutex_unlock(&arena->lock);

	return (true);
}

#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(arena_ralloc_junk_large_impl)
#endif
static void
arena_ralloc_junk_large(void *ptr, size_t old_usize, size_t usize)
{

	if (config_fill && unlikely(opt_junk)) {
		memset((void *)((uintptr_t)ptr + usize), 0x5a,
		    old_usize - usize);
	}
}
#ifdef JEMALLOC_JET
#undef arena_ralloc_junk_large
#define	arena_ralloc_junk_large JEMALLOC_N(arena_ralloc_junk_large)
arena_ralloc_junk_large_t *arena_ralloc_junk_large =
    JEMALLOC_N(arena_ralloc_junk_large_impl);
#endif

/*
 * Try to resize a large allocation, in order to avoid copying.  This will
 * always fail if growing an object, and the following run is already in use.
 */
static bool
arena_ralloc_large(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{
	size_t psize;

	psize = PAGE_CEILING(size + extra);
	if (psize == oldsize) {
		/* Same size class. */
		return (false);
	} else {
		arena_chunk_t *chunk;
		arena_t *arena;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
		arena = chunk->arena;

		if (psize < oldsize) {
			/* Fill before shrinking in order avoid a race. */
			arena_ralloc_junk_large(ptr, oldsize, psize);
			arena_ralloc_large_shrink(arena, chunk, ptr, oldsize,
			    psize);
			return (false);
		} else {
			bool ret = arena_ralloc_large_grow(arena, chunk, ptr,
			    oldsize, PAGE_CEILING(size),
			    psize - PAGE_CEILING(size), zero);
			if (config_fill && !ret && !zero) {
				if (unlikely(opt_junk)) {
					memset((void *)((uintptr_t)ptr +
					    oldsize), 0xa5, isalloc(ptr,
					    config_prof) - oldsize);
				} else if (unlikely(opt_zero)) {
					memset((void *)((uintptr_t)ptr +
					    oldsize), 0, isalloc(ptr,
					    config_prof) - oldsize);
				}
			}
			return (ret);
		}
	}
}

bool
arena_ralloc_no_move(void *ptr, size_t oldsize, size_t size, size_t extra,
    bool zero)
{

	/*
	 * Avoid moving the allocation if the size class can be left the same.
	 */
	if (oldsize <= arena_maxclass) {
		if (oldsize <= SMALL_MAXCLASS) {
			assert(arena_bin_info[small_size2bin(oldsize)].reg_size
			    == oldsize);
			if ((size + extra <= SMALL_MAXCLASS &&
			    small_size2bin(size + extra) ==
			    small_size2bin(oldsize)) || (size <= oldsize &&
			    size + extra >= oldsize))
				return (false);
		} else {
			assert(size <= arena_maxclass);
			if (size + extra > SMALL_MAXCLASS) {
				if (!arena_ralloc_large(ptr, oldsize, size,
				    extra, zero))
					return (false);
			}
		}
	}

	/* Reallocation would require a move. */
	return (true);
}

void *
arena_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize, size_t size,
    size_t extra, size_t alignment, bool zero, bool try_tcache_alloc,
    bool try_tcache_dalloc)
{
	void *ret;
	size_t copysize;

	/* Try to avoid moving the allocation. */
	if (!arena_ralloc_no_move(ptr, oldsize, size, extra, zero))
		return (ptr);

	/*
	 * size and oldsize are different enough that we need to move the
	 * object.  In that case, fall back to allocating new space and
	 * copying.
	 */
	if (alignment != 0) {
		size_t usize = sa2u(size + extra, alignment);
		if (usize == 0)
			return (NULL);
		ret = ipalloct(tsd, usize, alignment, zero, try_tcache_alloc,
		    arena);
	} else {
		ret = arena_malloc(tsd, arena, size + extra, zero,
		    try_tcache_alloc);
	}

	if (ret == NULL) {
		if (extra == 0)
			return (NULL);
		/* Try again, this time without extra. */
		if (alignment != 0) {
			size_t usize = sa2u(size, alignment);
			if (usize == 0)
				return (NULL);
			ret = ipalloct(tsd, usize, alignment, zero,
			    try_tcache_alloc, arena);
		} else {
			ret = arena_malloc(tsd, arena, size, zero,
			    try_tcache_alloc);
		}

		if (ret == NULL)
			return (NULL);
	}

	/* Junk/zero-filling were already done by ipalloc()/arena_malloc(). */

	/*
	 * Copy at most size bytes (not size+extra), since the caller has no
	 * expectation that the extra bytes will be reliably preserved.
	 */
	copysize = (size < oldsize) ? size : oldsize;
	JEMALLOC_VALGRIND_MAKE_MEM_UNDEFINED(ret, copysize);
	memcpy(ret, ptr, copysize);
	iqalloc(tsd, ptr, try_tcache_dalloc);
	return (ret);
}

dss_prec_t
arena_dss_prec_get(arena_t *arena)
{
	dss_prec_t ret;

	malloc_mutex_lock(&arena->lock);
	ret = arena->dss_prec;
	malloc_mutex_unlock(&arena->lock);
	return (ret);
}

bool
arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec)
{

	if (!have_dss)
		return (dss_prec != dss_prec_disabled);
	malloc_mutex_lock(&arena->lock);
	arena->dss_prec = dss_prec;
	malloc_mutex_unlock(&arena->lock);
	return (false);
}

void
arena_stats_merge(arena_t *arena, const char **dss, size_t *nactive,
    size_t *ndirty, arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats)
{
	unsigned i;

	malloc_mutex_lock(&arena->lock);
	*dss = dss_prec_names[arena->dss_prec];
	*nactive += arena->nactive;
	*ndirty += arena->ndirty;

	astats->mapped += arena->stats.mapped;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;
	astats->nrequests_large += arena->stats.nrequests_large;
	astats->allocated_huge += arena->stats.allocated_huge;
	astats->nmalloc_huge += arena->stats.nmalloc_huge;
	astats->ndalloc_huge += arena->stats.ndalloc_huge;
	astats->nrequests_huge += arena->stats.nrequests_huge;

	for (i = 0; i < nlclasses; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}
	malloc_mutex_unlock(&arena->lock);

	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];

		malloc_mutex_lock(&bin->lock);
		bstats[i].allocated += bin->stats.allocated;
		bstats[i].nmalloc += bin->stats.nmalloc;
		bstats[i].ndalloc += bin->stats.ndalloc;
		bstats[i].nrequests += bin->stats.nrequests;
		if (config_tcache) {
			bstats[i].nfills += bin->stats.nfills;
			bstats[i].nflushes += bin->stats.nflushes;
		}
		bstats[i].nruns += bin->stats.nruns;
		bstats[i].reruns += bin->stats.reruns;
		bstats[i].curruns += bin->stats.curruns;
		malloc_mutex_unlock(&bin->lock);
	}
}

bool
arena_new(arena_t *arena, unsigned ind)
{
	unsigned i;
	arena_bin_t *bin;

	arena->ind = ind;
	arena->nthreads = 0;
	arena->chunk_alloc = chunk_alloc_default;
	arena->chunk_dalloc = chunk_dalloc_default;

	if (malloc_mutex_init(&arena->lock))
		return (true);

	if (config_stats) {
		memset(&arena->stats, 0, sizeof(arena_stats_t));
		arena->stats.lstats =
		    (malloc_large_stats_t *)base_alloc(nlclasses *
		    sizeof(malloc_large_stats_t));
		if (arena->stats.lstats == NULL)
			return (true);
		memset(arena->stats.lstats, 0, nlclasses *
		    sizeof(malloc_large_stats_t));
		if (config_tcache)
			ql_new(&arena->tcache_ql);
	}

	if (config_prof)
		arena->prof_accumbytes = 0;

	arena->dss_prec = chunk_dss_prec_get();

	arena->spare = NULL;

	arena->nactive = 0;
	arena->ndirty = 0;

	arena_avail_tree_new(&arena->runs_avail);
	ql_new(&arena->runs_dirty);

	/* Initialize bins. */
	for (i = 0; i < NBINS; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);
		if (config_stats)
			memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
	}

	return (false);
}

/*
 * Calculate bin_info->run_size such that it meets the following constraints:
 *
 *   *) bin_info->run_size <= arena_maxclass
 *   *) bin_info->nregs <= RUN_MAXREGS
 *
 * bin_info->nregs and bin_info->reg0_offset are also calculated here, since
 * these settings are all interdependent.
 */
static void
bin_info_run_size_calc(arena_bin_info_t *bin_info)
{
	size_t pad_size;
	size_t try_run_size, perfect_run_size, actual_run_size;
	uint32_t try_nregs, perfect_nregs, actual_nregs;

	/*
	 * Determine redzone size based on minimum alignment and minimum
	 * redzone size.  Add padding to the end of the run if it is needed to
	 * align the regions.  The padding allows each redzone to be half the
	 * minimum alignment; without the padding, each redzone would have to
	 * be twice as large in order to maintain alignment.
	 */
	if (config_fill && unlikely(opt_redzone)) {
		size_t align_min = ZU(1) << (jemalloc_ffs(bin_info->reg_size) -
		    1);
		if (align_min <= REDZONE_MINSIZE) {
			bin_info->redzone_size = REDZONE_MINSIZE;
			pad_size = 0;
		} else {
			bin_info->redzone_size = align_min >> 1;
			pad_size = bin_info->redzone_size;
		}
	} else {
		bin_info->redzone_size = 0;
		pad_size = 0;
	}
	bin_info->reg_interval = bin_info->reg_size +
	    (bin_info->redzone_size << 1);

	/*
	 * Compute run size under ideal conditions (no redzones, no limit on run
	 * size).
	 */
	try_run_size = PAGE;
	try_nregs = try_run_size / bin_info->reg_size;
	do {
		perfect_run_size = try_run_size;
		perfect_nregs = try_nregs;

		try_run_size += PAGE;
		try_nregs = try_run_size / bin_info->reg_size;
	} while (perfect_run_size != perfect_nregs * bin_info->reg_size);
	assert(perfect_nregs <= RUN_MAXREGS);

	actual_run_size = perfect_run_size;
	actual_nregs = (actual_run_size - pad_size) / bin_info->reg_interval;

	/*
	 * Redzones can require enough padding that not even a single region can
	 * fit within the number of pages that would normally be dedicated to a
	 * run for this size class.  Increase the run size until at least one
	 * region fits.
	 */
	while (actual_nregs == 0) {
		assert(config_fill && unlikely(opt_redzone));

		actual_run_size += PAGE;
		actual_nregs = (actual_run_size - pad_size) /
		    bin_info->reg_interval;
	}

	/*
	 * Make sure that the run will fit within an arena chunk.
	 */
	while (actual_run_size > arena_maxclass) {
		actual_run_size -= PAGE;
		actual_nregs = (actual_run_size - pad_size) /
		    bin_info->reg_interval;
	}
	assert(actual_nregs > 0);

	/* Copy final settings. */
	bin_info->run_size = actual_run_size;
	bin_info->nregs = actual_nregs;
	bin_info->reg0_offset = actual_run_size - (actual_nregs *
	    bin_info->reg_interval) - pad_size + bin_info->redzone_size;

	assert(bin_info->reg0_offset - bin_info->redzone_size + (bin_info->nregs
	    * bin_info->reg_interval) + pad_size == bin_info->run_size);
}

static void
bin_info_init(void)
{
	arena_bin_info_t *bin_info;

#define	BIN_INFO_INIT_bin_yes(index, size) \
	bin_info = &arena_bin_info[index];				\
	bin_info->reg_size = size;					\
	bin_info_run_size_calc(bin_info);				\
	bitmap_info_init(&bin_info->bitmap_info, bin_info->nregs);
#define	BIN_INFO_INIT_bin_no(index, size)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup)	\
	BIN_INFO_INIT_bin_##bin(index, (ZU(1)<<lg_grp) + (ZU(ndelta)<<lg_delta))
	SIZE_CLASSES
#undef BIN_INFO_INIT_bin_yes
#undef BIN_INFO_INIT_bin_no
#undef SC
}

void
arena_boot(void)
{
	size_t header_size;
	unsigned i;

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
		header_size = offsetof(arena_chunk_t, map_bits) +
		    ((sizeof(arena_chunk_map_bits_t) +
		    sizeof(arena_chunk_map_misc_t)) * (chunk_npages-map_bias));
		map_bias = (header_size + PAGE_MASK) >> LG_PAGE;
	}
	assert(map_bias > 0);

	map_misc_offset = offsetof(arena_chunk_t, map_bits) +
	    sizeof(arena_chunk_map_bits_t) * (chunk_npages-map_bias);

	arena_maxclass = chunksize - (map_bias << LG_PAGE);

	bin_info_init();
}

void
arena_prefork(arena_t *arena)
{
	unsigned i;

	malloc_mutex_prefork(&arena->lock);
	for (i = 0; i < NBINS; i++)
		malloc_mutex_prefork(&arena->bins[i].lock);
}

void
arena_postfork_parent(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_parent(&arena->bins[i].lock);
	malloc_mutex_postfork_parent(&arena->lock);
}

void
arena_postfork_child(arena_t *arena)
{
	unsigned i;

	for (i = 0; i < NBINS; i++)
		malloc_mutex_postfork_child(&arena->bins[i].lock);
	malloc_mutex_postfork_child(&arena->lock);
}
