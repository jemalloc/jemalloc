#define	JEMALLOC_ARENA_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_qspace_max = LG_QSPACE_MAX_DEFAULT;
size_t	opt_lg_cspace_max = LG_CSPACE_MAX_DEFAULT;
ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
uint8_t const	*small_size2bin;

/* Various bin-related settings. */
unsigned	nqbins;
unsigned	ncbins;
unsigned	nsbins;
unsigned	nbins;
size_t		qspace_max;
size_t		cspace_min;
size_t		cspace_max;
size_t		sspace_min;
size_t		sspace_max;

size_t		lg_mspace;
size_t		mspace_mask;

/*
 * const_small_size2bin is a static constant lookup table that in the common
 * case can be used as-is for small_size2bin.  For dynamically linked programs,
 * this avoids a page of memory overhead per process.
 */
#define	S2B_1(i)	i,
#define	S2B_2(i)	S2B_1(i) S2B_1(i)
#define	S2B_4(i)	S2B_2(i) S2B_2(i)
#define	S2B_8(i)	S2B_4(i) S2B_4(i)
#define	S2B_16(i)	S2B_8(i) S2B_8(i)
#define	S2B_32(i)	S2B_16(i) S2B_16(i)
#define	S2B_64(i)	S2B_32(i) S2B_32(i)
#define	S2B_128(i)	S2B_64(i) S2B_64(i)
#define	S2B_256(i)	S2B_128(i) S2B_128(i)
/*
 * The number of elements in const_small_size2bin is dependent on page size
 * and on the definition for SUBPAGE.  If SUBPAGE changes, the '- 255' must also
 * change, along with the addition/removal of static lookup table element
 * definitions.
 */
static const uint8_t	const_small_size2bin[STATIC_PAGE_SIZE - 255] = {
	S2B_1(0xffU)		/*    0 */
#if (LG_QUANTUM == 4)
/* 16-byte quantum **********************/
#  ifdef JEMALLOC_TINY
#    if (LG_TINY_MIN == 2)
       S2B_4(0)			/*    4 */
       S2B_4(1)			/*    8 */
       S2B_8(2)			/*   16 */
#      define S2B_QMIN 2
#    elif (LG_TINY_MIN == 3)
       S2B_8(0)			/*    8 */
       S2B_8(1)			/*   16 */
#      define S2B_QMIN 1
#    else
#      error "Unsupported LG_TINY_MIN"
#    endif
#  else
	S2B_16(0)		/*   16 */
#    define S2B_QMIN 0
#  endif
	S2B_16(S2B_QMIN + 1)	/*   32 */
	S2B_16(S2B_QMIN + 2)	/*   48 */
	S2B_16(S2B_QMIN + 3)	/*   64 */
	S2B_16(S2B_QMIN + 4)	/*   80 */
	S2B_16(S2B_QMIN + 5)	/*   96 */
	S2B_16(S2B_QMIN + 6)	/*  112 */
	S2B_16(S2B_QMIN + 7)	/*  128 */
#  define S2B_CMIN (S2B_QMIN + 8)
#else
/* 8-byte quantum ***********************/
#  ifdef JEMALLOC_TINY
#    if (LG_TINY_MIN == 2)
       S2B_4(0)			/*    4 */
       S2B_4(1)			/*    8 */
#      define S2B_QMIN 1
#    else
#      error "Unsupported LG_TINY_MIN"
#    endif
#  else
	S2B_8(0)		/*    8 */
#    define S2B_QMIN 0
#  endif
	S2B_8(S2B_QMIN + 1)	/*   16 */
	S2B_8(S2B_QMIN + 2)	/*   24 */
	S2B_8(S2B_QMIN + 3)	/*   32 */
	S2B_8(S2B_QMIN + 4)	/*   40 */
	S2B_8(S2B_QMIN + 5)	/*   48 */
	S2B_8(S2B_QMIN + 6)	/*   56 */
	S2B_8(S2B_QMIN + 7)	/*   64 */
	S2B_8(S2B_QMIN + 8)	/*   72 */
	S2B_8(S2B_QMIN + 9)	/*   80 */
	S2B_8(S2B_QMIN + 10)	/*   88 */
	S2B_8(S2B_QMIN + 11)	/*   96 */
	S2B_8(S2B_QMIN + 12)	/*  104 */
	S2B_8(S2B_QMIN + 13)	/*  112 */
	S2B_8(S2B_QMIN + 14)	/*  120 */
	S2B_8(S2B_QMIN + 15)	/*  128 */
#  define S2B_CMIN (S2B_QMIN + 16)
#endif
/****************************************/
	S2B_64(S2B_CMIN + 0)	/*  192 */
	S2B_64(S2B_CMIN + 1)	/*  256 */
	S2B_64(S2B_CMIN + 2)	/*  320 */
	S2B_64(S2B_CMIN + 3)	/*  384 */
	S2B_64(S2B_CMIN + 4)	/*  448 */
	S2B_64(S2B_CMIN + 5)	/*  512 */
#  define S2B_SMIN (S2B_CMIN + 6)
	S2B_256(S2B_SMIN + 0)	/*  768 */
	S2B_256(S2B_SMIN + 1)	/* 1024 */
	S2B_256(S2B_SMIN + 2)	/* 1280 */
	S2B_256(S2B_SMIN + 3)	/* 1536 */
	S2B_256(S2B_SMIN + 4)	/* 1792 */
	S2B_256(S2B_SMIN + 5)	/* 2048 */
	S2B_256(S2B_SMIN + 6)	/* 2304 */
	S2B_256(S2B_SMIN + 7)	/* 2560 */
	S2B_256(S2B_SMIN + 8)	/* 2816 */
	S2B_256(S2B_SMIN + 9)	/* 3072 */
	S2B_256(S2B_SMIN + 10)	/* 3328 */
	S2B_256(S2B_SMIN + 11)	/* 3584 */
	S2B_256(S2B_SMIN + 12)	/* 3840 */
#if (STATIC_PAGE_SHIFT == 13)
	S2B_256(S2B_SMIN + 13)	/* 4096 */
	S2B_256(S2B_SMIN + 14)	/* 4352 */
	S2B_256(S2B_SMIN + 15)	/* 4608 */
	S2B_256(S2B_SMIN + 16)	/* 4864 */
	S2B_256(S2B_SMIN + 17)	/* 5120 */
	S2B_256(S2B_SMIN + 18)	/* 5376 */
	S2B_256(S2B_SMIN + 19)	/* 5632 */
	S2B_256(S2B_SMIN + 20)	/* 5888 */
	S2B_256(S2B_SMIN + 21)	/* 6144 */
	S2B_256(S2B_SMIN + 22)	/* 6400 */
	S2B_256(S2B_SMIN + 23)	/* 6656 */
	S2B_256(S2B_SMIN + 24)	/* 6912 */
	S2B_256(S2B_SMIN + 25)	/* 7168 */
	S2B_256(S2B_SMIN + 26)	/* 7424 */
	S2B_256(S2B_SMIN + 27)	/* 7680 */
	S2B_256(S2B_SMIN + 28)	/* 7936 */
#endif
};
#undef S2B_1
#undef S2B_2
#undef S2B_4
#undef S2B_8
#undef S2B_16
#undef S2B_32
#undef S2B_64
#undef S2B_128
#undef S2B_256
#undef S2B_QMIN
#undef S2B_CMIN
#undef S2B_SMIN

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	arena_run_split(arena_t *arena, arena_run_t *run, size_t size,
    bool large, bool zero);
static arena_chunk_t *arena_chunk_alloc(arena_t *arena);
static void	arena_chunk_dealloc(arena_t *arena, arena_chunk_t *chunk);
static arena_run_t *arena_run_alloc(arena_t *arena, size_t size, bool large,
    bool zero);
static void	arena_purge(arena_t *arena);
static void	arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty);
static void	arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, size_t oldsize, size_t newsize);
static void	arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, size_t oldsize, size_t newsize, bool dirty);
static arena_run_t *arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin);
static void	*arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin);
static size_t	arena_bin_run_size_calc(arena_bin_t *bin, size_t min_run_size);
static void	arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
static void	arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t size, size_t oldsize);
static bool	arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t size, size_t oldsize);
static bool	arena_ralloc_large(void *ptr, size_t size, size_t oldsize);
#ifdef JEMALLOC_TINY
static size_t	pow2_ceil(size_t x);
#endif
static bool	small_size2bin_init(void);
#ifdef JEMALLOC_DEBUG
static void	small_size2bin_validate(void);
#endif
static bool	small_size2bin_init_hard(void);

/******************************************************************************/

static inline int
arena_run_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	uintptr_t a_mapelm = (uintptr_t)a;
	uintptr_t b_mapelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_mapelm > b_mapelm) - (a_mapelm < b_mapelm));
}

/* Generate red-black tree functions. */
rb_gen(static JEMALLOC_ATTR(unused), arena_run_tree_, arena_run_tree_t,
    arena_chunk_map_t, u.rb_link, arena_run_comp)

static inline int
arena_avail_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	int ret;
	size_t a_size = a->bits & ~PAGE_MASK;
	size_t b_size = b->bits & ~PAGE_MASK;

	assert((a->bits & CHUNK_MAP_KEY) == CHUNK_MAP_KEY || (a->bits &
	    CHUNK_MAP_DIRTY) == (b->bits & CHUNK_MAP_DIRTY));

	ret = (a_size > b_size) - (a_size < b_size);
	if (ret == 0) {
		uintptr_t a_mapelm, b_mapelm;

		if ((a->bits & CHUNK_MAP_KEY) != CHUNK_MAP_KEY)
			a_mapelm = (uintptr_t)a;
		else {
			/*
			 * Treat keys as though they are lower than anything
			 * else.
			 */
			a_mapelm = 0;
		}
		b_mapelm = (uintptr_t)b;

		ret = (a_mapelm > b_mapelm) - (a_mapelm < b_mapelm);
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(static JEMALLOC_ATTR(unused), arena_avail_tree_, arena_avail_tree_t,
    arena_chunk_map_t, u.rb_link, arena_avail_comp)

static inline void *
arena_run_reg_alloc(arena_run_t *run, arena_bin_t *bin)
{
	void *ret;

	assert(run->magic == ARENA_RUN_MAGIC);
	assert(run->nfree > 0);

	run->nfree--;
	ret = run->avail;
	if (ret != NULL) {
		run->avail = *(void **)ret;
		/* Double free can cause assertion failure.*/
		assert(ret != NULL);
		/* Write-after free can cause assertion failure. */
		assert((uintptr_t)ret >= (uintptr_t)run +
		    (uintptr_t)bin->reg0_offset);
		assert((uintptr_t)ret < (uintptr_t)run->next);
		assert(((uintptr_t)ret - ((uintptr_t)run +
		    (uintptr_t)bin->reg0_offset)) % (uintptr_t)bin->reg_size ==
		    0);
		return (ret);
	}
	ret = run->next;
	run->next = (void *)((uintptr_t)ret + (uintptr_t)bin->reg_size);
	assert(ret != NULL);
	return (ret);
}

static inline void
arena_run_reg_dalloc(arena_run_t *run, void *ptr)
{

	assert(run->nfree < run->bin->nregs);
	/* Freeing an interior pointer can cause assertion failure. */
	assert(((uintptr_t)ptr - ((uintptr_t)run +
	    (uintptr_t)run->bin->reg0_offset)) % (uintptr_t)run->bin->reg_size
	    == 0);

	*(void **)ptr = run->avail;
	run->avail = ptr;
	run->nfree++;
}

static void
arena_run_split(arena_t *arena, arena_run_t *run, size_t size, bool large,
    bool zero)
{
	arena_chunk_t *chunk;
	size_t old_ndirty, run_ind, total_pages, need_pages, rem_pages, i;
	size_t flag_dirty;
	arena_avail_tree_t *runs_avail;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	old_ndirty = chunk->ndirty;
	run_ind = (unsigned)(((uintptr_t)run - (uintptr_t)chunk)
	    >> PAGE_SHIFT);
	flag_dirty = chunk->map[run_ind].bits & CHUNK_MAP_DIRTY;
	runs_avail = (flag_dirty != 0) ? &arena->runs_avail_dirty :
	    &arena->runs_avail_clean;
	total_pages = (chunk->map[run_ind].bits & ~PAGE_MASK) >>
	    PAGE_SHIFT;
	assert((chunk->map[run_ind+total_pages-1].bits & CHUNK_MAP_DIRTY) ==
	    flag_dirty);
	need_pages = (size >> PAGE_SHIFT);
	assert(need_pages > 0);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_tree_remove(runs_avail, &chunk->map[run_ind]);
	arena->nactive += need_pages;

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		if (flag_dirty != 0) {
			chunk->map[run_ind+need_pages].bits = (rem_pages <<
			    PAGE_SHIFT) | CHUNK_MAP_DIRTY;
			chunk->map[run_ind+total_pages-1].bits = (rem_pages <<
			    PAGE_SHIFT) | CHUNK_MAP_DIRTY;
		} else {
			chunk->map[run_ind+need_pages].bits = (rem_pages <<
			    PAGE_SHIFT) | (chunk->map[run_ind+need_pages].bits &
			    CHUNK_MAP_ZEROED);
			chunk->map[run_ind+total_pages-1].bits = (rem_pages <<
			    PAGE_SHIFT) |
			    (chunk->map[run_ind+total_pages-1].bits &
			    CHUNK_MAP_ZEROED);
		}
		arena_avail_tree_insert(runs_avail,
		    &chunk->map[run_ind+need_pages]);
	}

	/* Update dirty page accounting. */
	if (flag_dirty != 0) {
		chunk->ndirty -= need_pages;
		arena->ndirty -= need_pages;
	}

	/*
	 * Update the page map separately for large vs. small runs, since it is
	 * possible to avoid iteration for large mallocs.
	 */
	if (large) {
		if (zero) {
			if (flag_dirty == 0) {
				/*
				 * The run is clean, so some pages may be
				 * zeroed (i.e. never before touched).
				 */
				for (i = 0; i < need_pages; i++) {
					if ((chunk->map[run_ind + i].bits &
					    CHUNK_MAP_ZEROED) == 0) {
						memset((void *)((uintptr_t)
						    chunk + ((run_ind + i) <<
						    PAGE_SHIFT)), 0,
						    PAGE_SIZE);
					}
				}
			} else {
				/*
				 * The run is dirty, so all pages must be
				 * zeroed.
				 */
				memset((void *)((uintptr_t)chunk + (run_ind <<
				    PAGE_SHIFT)), 0, (need_pages <<
				    PAGE_SHIFT));
			}
		}

		/*
		 * Set the last element first, in case the run only contains one
		 * page (i.e. both statements set the same element).
		 */
		chunk->map[run_ind+need_pages-1].bits = CHUNK_MAP_LARGE |
		    CHUNK_MAP_ALLOCATED | flag_dirty;
		chunk->map[run_ind].bits = size | CHUNK_MAP_LARGE |
#ifdef JEMALLOC_PROF
		    CHUNK_MAP_CLASS_MASK |
#endif
		    CHUNK_MAP_ALLOCATED | flag_dirty;
	} else {
		assert(zero == false);
		/*
		 * Propagate the dirty flag to the allocated small run, so that
		 * arena_dalloc_bin_run() has the ability to conditionally trim
		 * clean pages.
		 */
		chunk->map[run_ind].bits = CHUNK_MAP_ALLOCATED | flag_dirty;
		for (i = 1; i < need_pages - 1; i++) {
			chunk->map[run_ind + i].bits = (i << PAGE_SHIFT)
			    | CHUNK_MAP_ALLOCATED;
		}
		chunk->map[run_ind + need_pages - 1].bits = ((need_pages - 1) <<
		    PAGE_SHIFT) | CHUNK_MAP_ALLOCATED | flag_dirty;
	}
}

static arena_chunk_t *
arena_chunk_alloc(arena_t *arena)
{
	arena_chunk_t *chunk;
	size_t i;

	if (arena->spare != NULL) {
		arena_avail_tree_t *runs_avail;

		chunk = arena->spare;
		arena->spare = NULL;

		/* Insert the run into the appropriate runs_avail_* tree. */
		if ((chunk->map[arena_chunk_header_npages].bits &
		    CHUNK_MAP_DIRTY) == 0)
			runs_avail = &arena->runs_avail_clean;
		else
			runs_avail = &arena->runs_avail_dirty;
		arena_avail_tree_insert(runs_avail,
		    &chunk->map[arena_chunk_header_npages]);
	} else {
		bool zero;
		size_t zeroed;

		zero = false;
		malloc_mutex_unlock(&arena->lock);
		chunk = (arena_chunk_t *)chunk_alloc(chunksize, &zero);
		malloc_mutex_lock(&arena->lock);
		if (chunk == NULL)
			return (NULL);
#ifdef JEMALLOC_STATS
		arena->stats.mapped += chunksize;
#endif

		chunk->arena = arena;
		ql_elm_new(chunk, link_dirty);
		chunk->dirtied = false;

		/*
		 * Claim that no pages are in use, since the header is merely
		 * overhead.
		 */
		chunk->ndirty = 0;

		/*
		 * Initialize the map to contain one maximal free untouched run.
		 * Mark the pages as zeroed iff chunk_alloc() returned a zeroed
		 * chunk.
		 */
		zeroed = zero ? CHUNK_MAP_ZEROED : 0;
		for (i = 0; i < arena_chunk_header_npages; i++)
			chunk->map[i].bits = 0;
		chunk->map[i].bits = arena_maxclass | zeroed;
		for (i++; i < chunk_npages-1; i++)
			chunk->map[i].bits = zeroed;
		chunk->map[chunk_npages-1].bits = arena_maxclass | zeroed;

		/* Insert the run into the runs_avail_clean tree. */
		arena_avail_tree_insert(&arena->runs_avail_clean,
		    &chunk->map[arena_chunk_header_npages]);
	}

	return (chunk);
}

static void
arena_chunk_dealloc(arena_t *arena, arena_chunk_t *chunk)
{
	arena_avail_tree_t *runs_avail;

	while (arena->spare != NULL) {
		arena_chunk_t *spare = arena->spare;

		arena->spare = NULL;
		if (spare->dirtied) {
			ql_remove(&chunk->arena->chunks_dirty, spare,
			    link_dirty);
			arena->ndirty -= spare->ndirty;
		}
		malloc_mutex_unlock(&arena->lock);
		chunk_dealloc((void *)spare, chunksize);
		malloc_mutex_lock(&arena->lock);
#ifdef JEMALLOC_STATS
		arena->stats.mapped -= chunksize;
#endif
	}

	/*
	 * Remove run from the appropriate runs_avail_* tree, so that the arena
	 * does not use it.
	 */
	if ((chunk->map[arena_chunk_header_npages].bits &
	    CHUNK_MAP_DIRTY) == 0)
		runs_avail = &arena->runs_avail_clean;
	else
		runs_avail = &arena->runs_avail_dirty;
	arena_avail_tree_remove(runs_avail,
	    &chunk->map[arena_chunk_header_npages]);

	arena->spare = chunk;
}

static arena_run_t *
arena_run_alloc(arena_t *arena, size_t size, bool large, bool zero)
{
	arena_chunk_t *chunk;
	arena_run_t *run;
	arena_chunk_map_t *mapelm, key;

	assert(size <= arena_maxclass);
	assert((size & PAGE_MASK) == 0);

	/* Search the arena's chunks for the lowest best fit. */
	key.bits = size | CHUNK_MAP_KEY;
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_dirty, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)run_chunk->map)
		    / sizeof(arena_chunk_map_t);

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_clean, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)run_chunk->map)
		    / sizeof(arena_chunk_map_t);

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk != NULL) {
		run = (arena_run_t *)((uintptr_t)chunk +
		    (arena_chunk_header_npages << PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	/*
	 * arena_chunk_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dropped arena->lock in
	 * arena_chunk_alloc(), so search one more time.
	 */
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_dirty, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)run_chunk->map)
		    / sizeof(arena_chunk_map_t);

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail_clean, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)run_chunk->map)
		    / sizeof(arena_chunk_map_t);

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind <<
		    PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	return (NULL);
}

static inline void
arena_maybe_purge(arena_t *arena)
{

	/* Enforce opt_lg_dirty_mult. */
	if (opt_lg_dirty_mult >= 0 && arena->ndirty > arena->npurgatory &&
	    (arena->ndirty - arena->npurgatory) > chunk_npages &&
	    (arena->nactive >> opt_lg_dirty_mult) < (arena->ndirty -
	    arena->npurgatory))
		arena_purge(arena);
}

static inline void
arena_chunk_purge(arena_t *arena, arena_chunk_t *chunk)
{
	ql_head(arena_chunk_map_t) mapelms;
	arena_chunk_map_t *mapelm;
	size_t pageind, flag_zeroed;
#ifdef JEMALLOC_DEBUG
	size_t ndirty;
#endif
#ifdef JEMALLOC_STATS
	size_t nmadvise;
#endif

	ql_new(&mapelms);

	flag_zeroed =
#ifdef JEMALLOC_SWAP
	    swap_enabled ? 0 :
#endif
	    CHUNK_MAP_ZEROED;

	/*
	 * If chunk is the spare, temporarily re-allocate it, 1) so that its
	 * run is reinserted into runs_avail_dirty, and 2) so that it cannot be
	 * completely discarded by another thread while arena->lock is dropped
	 * by this thread.  Note that the arena_run_dalloc() call will
	 * implicitly deallocate the chunk, so no explicit action is required
	 * in this function to deallocate the chunk.
	 *
	 * Note that once a chunk contains dirty pages, it cannot again contain
	 * a single run unless 1) it is a dirty run, or 2) this function purges
	 * dirty pages and causes the transition to a single clean run.  Thus
	 * (chunk == arena->spare) is possible, but it is not possible for
	 * this function to be called on the spare unless it contains a dirty
	 * run.
	 */
	if (chunk == arena->spare) {
		assert((chunk->map[arena_chunk_header_npages].bits &
		    CHUNK_MAP_DIRTY) != 0);
		arena_chunk_alloc(arena);
	}

	/* Temporarily allocate all free dirty runs within chunk. */
	for (pageind = arena_chunk_header_npages; pageind < chunk_npages;) {
		mapelm = &chunk->map[pageind];
		if ((mapelm->bits & CHUNK_MAP_ALLOCATED) == 0) {
			size_t npages;

			npages = mapelm->bits >> PAGE_SHIFT;
			assert(pageind + npages <= chunk_npages);
			if (mapelm->bits & CHUNK_MAP_DIRTY) {
				size_t i;

				arena_avail_tree_remove(
				    &arena->runs_avail_dirty, mapelm);

				/*
				 * Update internal elements in the page map, so
				 * that CHUNK_MAP_ZEROED is properly set.
				 * madvise(..., MADV_DONTNEED) results in
				 * zero-filled pages for anonymous mappings,
				 * but not for file-backed mappings.
				 */
				mapelm->bits = (npages << PAGE_SHIFT) |
				    CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED |
				    flag_zeroed;
				for (i = 1; i < npages - 1; i++) {
					chunk->map[pageind + i].bits =
					    flag_zeroed;
				}
				if (npages > 1) {
					chunk->map[pageind + npages - 1].bits =
					(npages << PAGE_SHIFT) |
					CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED |
					flag_zeroed;
				}

				arena->nactive += npages;
				/* Append to list for later processing. */
				ql_elm_new(mapelm, u.ql_link);
				ql_tail_insert(&mapelms, mapelm, u.ql_link);
			}

			pageind += npages;
		} else {
			/* Skip allocated run. */
			if (mapelm->bits & CHUNK_MAP_LARGE)
				pageind += mapelm->bits >> PAGE_SHIFT;
			else {
				arena_run_t *run = (arena_run_t *)((uintptr_t)
				    chunk + (uintptr_t)(pageind << PAGE_SHIFT));

				assert((mapelm->bits >> PAGE_SHIFT) == 0);
				assert(run->magic == ARENA_RUN_MAGIC);
				pageind += run->bin->run_size >> PAGE_SHIFT;
			}
		}
	}
	assert(pageind == chunk_npages);

#ifdef JEMALLOC_DEBUG
	ndirty = chunk->ndirty;
#endif
#ifdef JEMALLOC_STATS
	arena->stats.purged += chunk->ndirty;
#endif
	arena->ndirty -= chunk->ndirty;
	chunk->ndirty = 0;
	ql_remove(&arena->chunks_dirty, chunk, link_dirty);
	chunk->dirtied = false;

	malloc_mutex_unlock(&arena->lock);
#ifdef JEMALLOC_STATS
	nmadvise = 0;
#endif
	ql_foreach(mapelm, &mapelms, u.ql_link) {
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t);
		size_t npages = mapelm->bits >> PAGE_SHIFT;

		assert(pageind + npages <= chunk_npages);
#ifdef JEMALLOC_DEBUG
		assert(ndirty >= npages);
		ndirty -= npages;
#endif
		madvise((void *)((uintptr_t)chunk + (pageind << PAGE_SHIFT)),
		    (npages << PAGE_SHIFT), MADV_DONTNEED);
#ifdef JEMALLOC_STATS
		nmadvise++;
#endif
	}
#ifdef JEMALLOC_DEBUG
	assert(ndirty == 0);
#endif
	malloc_mutex_lock(&arena->lock);
#ifdef JEMALLOC_STATS
	arena->stats.nmadvise += nmadvise;
#endif

	/* Deallocate runs. */
	for (mapelm = ql_first(&mapelms); mapelm != NULL;
	    mapelm = ql_first(&mapelms)) {
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t);
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)(pageind << PAGE_SHIFT));

		ql_remove(&mapelms, mapelm, u.ql_link);
		arena_run_dalloc(arena, run, false);
	}
}

static void
arena_purge(arena_t *arena)
{
	arena_chunk_t *chunk;
	size_t npurgatory;
#ifdef JEMALLOC_DEBUG
	size_t ndirty = 0;

	ql_foreach(chunk, &arena->chunks_dirty, link_dirty) {
	    assert(chunk->dirtied);
	    ndirty += chunk->ndirty;
	}
	assert(ndirty == arena->ndirty);
#endif
	assert(arena->ndirty > arena->npurgatory);
	assert(arena->ndirty > chunk_npages);
	assert((arena->nactive >> opt_lg_dirty_mult) < arena->ndirty);

#ifdef JEMALLOC_STATS
	arena->stats.npurge++;
#endif

	/*
	 * Compute the minimum number of pages that this thread should try to
	 * purge, and add the result to arena->npurgatory.  This will keep
	 * multiple threads from racing to reduce ndirty below the threshold.
	 */
	npurgatory = (arena->ndirty - arena->npurgatory) - (arena->nactive >>
	    opt_lg_dirty_mult);
	arena->npurgatory += npurgatory;

	while (npurgatory > 0) {
		/* Get next chunk with dirty pages. */
		chunk = ql_first(&arena->chunks_dirty);
		if (chunk == NULL) {
			/*
			 * This thread was unable to purge as many pages as
			 * originally intended, due to races with other threads
			 * that either did some of the purging work, or re-used
			 * dirty pages.
			 */
			arena->npurgatory -= npurgatory;
			return;
		}
		while (chunk->ndirty == 0) {
			ql_remove(&arena->chunks_dirty, chunk, link_dirty);
			chunk->dirtied = false;
			chunk = ql_first(&arena->chunks_dirty);
			if (chunk == NULL) {
				/* Same logic as for above. */
				arena->npurgatory -= npurgatory;
				return;
			}
		}

		if (chunk->ndirty > npurgatory) {
			/*
			 * This thread will, at a minimum, purge all the dirty
			 * pages in chunk, so set npurgatory to reflect this
			 * thread's commitment to purge the pages.  This tends
			 * to reduce the chances of the following scenario:
			 *
			 * 1) This thread sets arena->npurgatory such that
			 *    (arena->ndirty - arena->npurgatory) is at the
			 *    threshold.
			 * 2) This thread drops arena->lock.
			 * 3) Another thread causes one or more pages to be
			 *    dirtied, and immediately determines that it must
			 *    purge dirty pages.
			 *
			 * If this scenario *does* play out, that's okay,
			 * because all of the purging work being done really
			 * needs to happen.
			 */
			arena->npurgatory += chunk->ndirty - npurgatory;
			npurgatory = chunk->ndirty;
		}

		arena->npurgatory -= chunk->ndirty;
		npurgatory -= chunk->ndirty;
		arena_chunk_purge(arena, chunk);
	}
}

static void
arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty)
{
	arena_chunk_t *chunk;
	size_t size, run_ind, run_pages, flag_dirty;
	arena_avail_tree_t *runs_avail;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	run_ind = (size_t)(((uintptr_t)run - (uintptr_t)chunk)
	    >> PAGE_SHIFT);
	assert(run_ind >= arena_chunk_header_npages);
	assert(run_ind < chunk_npages);
	if ((chunk->map[run_ind].bits & CHUNK_MAP_LARGE) != 0)
		size = chunk->map[run_ind].bits & ~PAGE_MASK;
	else
		size = run->bin->run_size;
	run_pages = (size >> PAGE_SHIFT);
	arena->nactive -= run_pages;

	/*
	 * The run is dirty if the caller claims to have dirtied it, as well as
	 * if it was already dirty before being allocated.
	 */
	if ((chunk->map[run_ind].bits & CHUNK_MAP_DIRTY) != 0)
		dirty = true;
	flag_dirty = dirty ? CHUNK_MAP_DIRTY : 0;
	runs_avail = dirty ? &arena->runs_avail_dirty :
	    &arena->runs_avail_clean;

	/* Mark pages as unallocated in the chunk map. */
	if (dirty) {
		chunk->map[run_ind].bits = size | flag_dirty;
		chunk->map[run_ind+run_pages-1].bits = size | flag_dirty;

		chunk->ndirty += run_pages;
		arena->ndirty += run_pages;
	} else {
		chunk->map[run_ind].bits = size | (chunk->map[run_ind].bits &
		    CHUNK_MAP_ZEROED);
		chunk->map[run_ind+run_pages-1].bits = size |
		    (chunk->map[run_ind+run_pages-1].bits & CHUNK_MAP_ZEROED);
	}

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    (chunk->map[run_ind+run_pages].bits & CHUNK_MAP_ALLOCATED) == 0 &&
	    (chunk->map[run_ind+run_pages].bits & CHUNK_MAP_DIRTY) ==
	    flag_dirty) {
		size_t nrun_size = chunk->map[run_ind+run_pages].bits &
		    ~PAGE_MASK;

		/*
		 * Remove successor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		arena_avail_tree_remove(runs_avail,
		    &chunk->map[run_ind+run_pages]);

		size += nrun_size;
		run_pages = size >> PAGE_SHIFT;

		assert((chunk->map[run_ind+run_pages-1].bits & ~PAGE_MASK)
		    == nrun_size);
		chunk->map[run_ind].bits = size | (chunk->map[run_ind].bits &
		    CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind+run_pages-1].bits = size |
		    (chunk->map[run_ind+run_pages-1].bits &
		    CHUNK_MAP_FLAGS_MASK);
	}

	/* Try to coalesce backward. */
	if (run_ind > arena_chunk_header_npages && (chunk->map[run_ind-1].bits &
	    CHUNK_MAP_ALLOCATED) == 0 && (chunk->map[run_ind-1].bits &
	    CHUNK_MAP_DIRTY) == flag_dirty) {
		size_t prun_size = chunk->map[run_ind-1].bits & ~PAGE_MASK;

		run_ind -= prun_size >> PAGE_SHIFT;

		/*
		 * Remove predecessor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		arena_avail_tree_remove(runs_avail, &chunk->map[run_ind]);

		size += prun_size;
		run_pages = size >> PAGE_SHIFT;

		assert((chunk->map[run_ind].bits & ~PAGE_MASK) == prun_size);
		chunk->map[run_ind].bits = size | (chunk->map[run_ind].bits &
		    CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind+run_pages-1].bits = size |
		    (chunk->map[run_ind+run_pages-1].bits &
		    CHUNK_MAP_FLAGS_MASK);
	}

	/* Insert into runs_avail, now that coalescing is complete. */
	arena_avail_tree_insert(runs_avail, &chunk->map[run_ind]);

	/*
	 * Deallocate chunk if it is now completely unused.  The bit
	 * manipulation checks whether the first run is unallocated and extends
	 * to the end of the chunk.
	 */
	if ((chunk->map[arena_chunk_header_npages].bits & (~PAGE_MASK |
	    CHUNK_MAP_ALLOCATED)) == arena_maxclass)
		arena_chunk_dealloc(arena, chunk);

	/*
	 * It is okay to do dirty page processing even if the chunk was
	 * deallocated above, since in that case it is the spare.  Waiting
	 * until after possible chunk deallocation to do dirty processing
	 * allows for an old spare to be fully deallocated, thus decreasing the
	 * chances of spuriously crossing the dirty page purging threshold.
	 */
	if (dirty) {
		if (chunk->dirtied == false) {
			ql_tail_insert(&arena->chunks_dirty, chunk, link_dirty);
			chunk->dirtied = true;
		}
		arena_maybe_purge(arena);
	}
}

static void
arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT;
	size_t head_npages = (oldsize - newsize) >> PAGE_SHIFT;
	size_t flags = chunk->map[pageind].bits & CHUNK_MAP_FLAGS_MASK;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.
	 */
	assert(chunk->map[pageind].bits & CHUNK_MAP_LARGE);
	assert(chunk->map[pageind].bits & CHUNK_MAP_ALLOCATED);
	chunk->map[pageind].bits = (oldsize - newsize) | flags;
	chunk->map[pageind+head_npages].bits = newsize | flags;

	arena_run_dalloc(arena, run, false);
}

static void
arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize, bool dirty)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT;
	size_t npages = newsize >> PAGE_SHIFT;
	size_t flags = chunk->map[pageind].bits & CHUNK_MAP_FLAGS_MASK;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.
	 */
	assert(chunk->map[pageind].bits & CHUNK_MAP_LARGE);
	assert(chunk->map[pageind].bits & CHUNK_MAP_ALLOCATED);
	chunk->map[pageind].bits = newsize | flags;
	chunk->map[pageind+npages-1].bits = newsize | flags;
	chunk->map[pageind+npages].bits = (oldsize - newsize) | flags;

	arena_run_dalloc(arena, (arena_run_t *)((uintptr_t)run + newsize),
	    dirty);
}

static arena_run_t *
arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin)
{
	arena_chunk_map_t *mapelm;
	arena_run_t *run;

	/* Look for a usable run. */
	mapelm = arena_run_tree_first(&bin->runs);
	if (mapelm != NULL) {
		arena_chunk_t *chunk;
		size_t pageind;

		/* run is guaranteed to have available space. */
		arena_run_tree_remove(&bin->runs, mapelm);

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(mapelm);
		pageind = (((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t));
		run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
		    (mapelm->bits >> PAGE_SHIFT))
		    << PAGE_SHIFT));
#ifdef JEMALLOC_STATS
		bin->stats.reruns++;
#endif
		return (run);
	}
	/* No existing runs have any space available. */

	/* Allocate a new run. */
	malloc_mutex_unlock(&bin->lock);
	/******************************/
	malloc_mutex_lock(&arena->lock);
	run = arena_run_alloc(arena, bin->run_size, false, false);
	if (run != NULL) {
		/* Initialize run internals. */
		run->bin = bin;
		run->avail = NULL;
		run->next = (void *)(((uintptr_t)run) +
		    (uintptr_t)bin->reg0_offset);
		run->nfree = bin->nregs;
#ifdef JEMALLOC_DEBUG
		run->magic = ARENA_RUN_MAGIC;
#endif
	}
	malloc_mutex_unlock(&arena->lock);
	/********************************/
	malloc_mutex_lock(&bin->lock);
	if (run != NULL) {
#ifdef JEMALLOC_STATS
		bin->stats.nruns++;
		bin->stats.curruns++;
		if (bin->stats.curruns > bin->stats.highruns)
			bin->stats.highruns = bin->stats.curruns;
#endif
		return (run);
	}

	/*
	 * arena_run_alloc() failed, but another thread may have made
	 * sufficient memory available while this one dopped bin->lock above,
	 * so search one more time.
	 */
	mapelm = arena_run_tree_first(&bin->runs);
	if (mapelm != NULL) {
		arena_chunk_t *chunk;
		size_t pageind;

		/* run is guaranteed to have available space. */
		arena_run_tree_remove(&bin->runs, mapelm);

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(mapelm);
		pageind = (((uintptr_t)mapelm - (uintptr_t)chunk->map) /
		    sizeof(arena_chunk_map_t));
		run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
		    (mapelm->bits >> PAGE_SHIFT))
		    << PAGE_SHIFT));
#ifdef JEMALLOC_STATS
		bin->stats.reruns++;
#endif
		return (run);
	}

	return (NULL);
}

/* Re-fill bin->runcur, then call arena_run_reg_alloc(). */
static void *
arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin)
{
	void *ret;
	arena_run_t *run;

	bin->runcur = NULL;
	run = arena_bin_nonfull_run_get(arena, bin);
	if (bin->runcur != NULL && bin->runcur->nfree > 0) {
		/*
		 * Another thread updated runcur while this one ran without the
		 * bin lock in arena_bin_nonfull_run_get().
		 */
		assert(bin->runcur->magic == ARENA_RUN_MAGIC);
		assert(bin->runcur->nfree > 0);
		ret = arena_run_reg_alloc(bin->runcur, bin);
		if (run != NULL) {
			malloc_mutex_unlock(&bin->lock);
			malloc_mutex_lock(&arena->lock);
			arena_run_dalloc(arena, run, false);
			malloc_mutex_unlock(&arena->lock);
			malloc_mutex_lock(&bin->lock);
		}
		return (ret);
	}

	if (run == NULL)
		return (NULL);

	bin->runcur = run;

	assert(bin->runcur->magic == ARENA_RUN_MAGIC);
	assert(bin->runcur->nfree > 0);

	return (arena_run_reg_alloc(bin->runcur, bin));
}

#ifdef JEMALLOC_PROF
void
arena_prof_accum(arena_t *arena, uint64_t accumbytes)
{

	if (prof_interval != 0) {
		arena->prof_accumbytes += accumbytes;
		if (arena->prof_accumbytes >= prof_interval) {
			prof_idump();
			arena->prof_accumbytes -= prof_interval;
		}
	}
}
#endif

#ifdef JEMALLOC_TCACHE
void
arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin, size_t binind
#  ifdef JEMALLOC_PROF
    , uint64_t prof_accumbytes
#  endif
    )
{
	unsigned i, nfill;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;

	assert(tbin->ncached == 0);

#ifdef JEMALLOC_PROF
	malloc_mutex_lock(&arena->lock);
	arena_prof_accum(arena, prof_accumbytes);
	malloc_mutex_unlock(&arena->lock);
#endif
	bin = &arena->bins[binind];
	malloc_mutex_lock(&bin->lock);
	for (i = 0, nfill = (tbin->ncached_max >> 1); i < nfill; i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_run_reg_alloc(run, bin);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL)
			break;
		*(void **)ptr = tbin->avail;
		tbin->avail = ptr;
	}
#ifdef JEMALLOC_STATS
	bin->stats.allocated += (i - tbin->ncached) * bin->reg_size;
	bin->stats.nmalloc += i;
	bin->stats.nrequests += tbin->tstats.nrequests;
	bin->stats.nfills++;
	tbin->tstats.nrequests = 0;
#endif
	malloc_mutex_unlock(&bin->lock);
	tbin->ncached = i;
	if (tbin->ncached > tbin->high_water)
		tbin->high_water = tbin->ncached;
}
#endif

/*
 * Calculate bin->run_size such that it meets the following constraints:
 *
 *   *) bin->run_size >= min_run_size
 *   *) bin->run_size <= arena_maxclass
 *   *) run header overhead <= RUN_MAX_OVRHD (or header overhead relaxed).
 *   *) run header size < PAGE_SIZE
 *
 * bin->nregs and bin->reg0_offset are also calculated here, since these
 * settings are all interdependent.
 */
static size_t
arena_bin_run_size_calc(arena_bin_t *bin, size_t min_run_size)
{
	size_t try_run_size, good_run_size;
	uint32_t try_nregs, good_nregs;
	uint32_t try_hdr_size, good_hdr_size;
#ifdef JEMALLOC_PROF
	uint32_t try_cnt0_offset, good_cnt0_offset;
#endif
	uint32_t try_reg0_offset, good_reg0_offset;

	assert(min_run_size >= PAGE_SIZE);
	assert(min_run_size <= arena_maxclass);

	/*
	 * Calculate known-valid settings before entering the run_size
	 * expansion loop, so that the first part of the loop always copies
	 * valid settings.
	 *
	 * The do..while loop iteratively reduces the number of regions until
	 * the run header and the regions no longer overlap.  A closed formula
	 * would be quite messy, since there is an interdependency between the
	 * header's mask length and the number of regions.
	 */
	try_run_size = min_run_size;
	try_nregs = ((try_run_size - sizeof(arena_run_t)) / bin->reg_size)
	    + 1; /* Counter-act try_nregs-- in loop. */
	do {
		try_nregs--;
		try_hdr_size = sizeof(arena_run_t);
#ifdef JEMALLOC_PROF
		if (opt_prof && prof_promote == false) {
			/* Pad to a quantum boundary. */
			try_hdr_size = QUANTUM_CEILING(try_hdr_size);
			try_cnt0_offset = try_hdr_size;
			/* Add space for one (prof_thr_cnt_t *) per region. */
			try_hdr_size += try_nregs * sizeof(prof_thr_cnt_t *);
		} else
			try_cnt0_offset = 0;
#endif
		try_reg0_offset = try_run_size - (try_nregs * bin->reg_size);
	} while (try_hdr_size > try_reg0_offset);

	/* run_size expansion loop. */
	do {
		/*
		 * Copy valid settings before trying more aggressive settings.
		 */
		good_run_size = try_run_size;
		good_nregs = try_nregs;
		good_hdr_size = try_hdr_size;
#ifdef JEMALLOC_PROF
		good_cnt0_offset = try_cnt0_offset;
#endif
		good_reg0_offset = try_reg0_offset;

		/* Try more aggressive settings. */
		try_run_size += PAGE_SIZE;
		try_nregs = ((try_run_size - sizeof(arena_run_t)) /
		    bin->reg_size) + 1; /* Counter-act try_nregs-- in loop. */
		do {
			try_nregs--;
			try_hdr_size = sizeof(arena_run_t);
#ifdef JEMALLOC_PROF
			if (opt_prof && prof_promote == false) {
				/* Pad to a quantum boundary. */
				try_hdr_size = QUANTUM_CEILING(try_hdr_size);
				try_cnt0_offset = try_hdr_size;
				/*
				 * Add space for one (prof_thr_cnt_t *) per
				 * region.
				 */
				try_hdr_size += try_nregs *
				    sizeof(prof_thr_cnt_t *);
			}
#endif
			try_reg0_offset = try_run_size - (try_nregs *
			    bin->reg_size);
		} while (try_hdr_size > try_reg0_offset);
	} while (try_run_size <= arena_maxclass
	    && try_run_size <= arena_maxclass
	    && RUN_MAX_OVRHD * (bin->reg_size << 3) > RUN_MAX_OVRHD_RELAX
	    && (try_reg0_offset << RUN_BFP) > RUN_MAX_OVRHD * try_run_size
	    && try_hdr_size < PAGE_SIZE);

	assert(good_hdr_size <= good_reg0_offset);

	/* Copy final settings. */
	bin->run_size = good_run_size;
	bin->nregs = good_nregs;
#ifdef JEMALLOC_PROF
	bin->cnt0_offset = good_cnt0_offset;
#endif
	bin->reg0_offset = good_reg0_offset;

	return (good_run_size);
}

void *
arena_malloc_small(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	arena_run_t *run;
	size_t binind;

	binind = small_size2bin[size];
	assert(binind < nbins);
	bin = &arena->bins[binind];
	size = bin->reg_size;

	malloc_mutex_lock(&bin->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_run_reg_alloc(run, bin);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&bin->lock);
		return (NULL);
	}

#ifdef JEMALLOC_STATS
	bin->stats.allocated += size;
	bin->stats.nmalloc++;
	bin->stats.nrequests++;
#endif
	malloc_mutex_unlock(&bin->lock);
#ifdef JEMALLOC_PROF
	if (isthreaded == false) {
		malloc_mutex_lock(&arena->lock);
		arena_prof_accum(arena, size);
		malloc_mutex_unlock(&arena->lock);
	}
#endif

	if (zero == false) {
#ifdef JEMALLOC_FILL
		if (opt_junk)
			memset(ret, 0xa5, size);
		else if (opt_zero)
			memset(ret, 0, size);
#endif
	} else
		memset(ret, 0, size);

	return (ret);
}

void *
arena_malloc_large(arena_t *arena, size_t size, bool zero)
{
	void *ret;

	/* Large allocation. */
	size = PAGE_CEILING(size);
	malloc_mutex_lock(&arena->lock);
	ret = (void *)arena_run_alloc(arena, size, true, zero);
	if (ret == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}
#ifdef JEMALLOC_STATS
	arena->stats.nmalloc_large++;
	arena->stats.nrequests_large++;
	arena->stats.allocated_large += size;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nmalloc++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nrequests++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns++;
	if (arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns >
	    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns) {
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns =
		    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns;
	}
#endif
#ifdef JEMALLOC_PROF
	arena_prof_accum(arena, size);
#endif
	malloc_mutex_unlock(&arena->lock);

	if (zero == false) {
#ifdef JEMALLOC_FILL
		if (opt_junk)
			memset(ret, 0xa5, size);
		else if (opt_zero)
			memset(ret, 0, size);
#endif
	}

	return (ret);
}

void *
arena_malloc(size_t size, bool zero)
{

	assert(size != 0);
	assert(QUANTUM_CEILING(size) <= arena_maxclass);

	if (size <= small_maxclass) {
#ifdef JEMALLOC_TCACHE
		tcache_t *tcache;

		if ((tcache = tcache_get()) != NULL)
			return (tcache_alloc_small(tcache, size, zero));
		else

#endif
			return (arena_malloc_small(choose_arena(), size, zero));
	} else {
#ifdef JEMALLOC_TCACHE
		if (size <= tcache_maxclass) {
			tcache_t *tcache;

			if ((tcache = tcache_get()) != NULL)
				return (tcache_alloc_large(tcache, size, zero));
			else {
				return (arena_malloc_large(choose_arena(),
				    size, zero));
			}
		} else
#endif
			return (arena_malloc_large(choose_arena(), size, zero));
	}
}

/* Only handles large allocations that require more than page alignment. */
void *
arena_palloc(arena_t *arena, size_t alignment, size_t size, size_t alloc_size)
{
	void *ret;
	size_t offset;
	arena_chunk_t *chunk;

	assert((size & PAGE_MASK) == 0);
	assert((alignment & PAGE_MASK) == 0);

	malloc_mutex_lock(&arena->lock);
	ret = (void *)arena_run_alloc(arena, alloc_size, true, false);
	if (ret == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ret);

	offset = (uintptr_t)ret & (alignment - 1);
	assert((offset & PAGE_MASK) == 0);
	assert(offset < alloc_size);
	if (offset == 0)
		arena_run_trim_tail(arena, chunk, ret, alloc_size, size, false);
	else {
		size_t leadsize, trailsize;

		leadsize = alignment - offset;
		if (leadsize > 0) {
			arena_run_trim_head(arena, chunk, ret, alloc_size,
			    alloc_size - leadsize);
			ret = (void *)((uintptr_t)ret + leadsize);
		}

		trailsize = alloc_size - leadsize - size;
		if (trailsize != 0) {
			/* Trim trailing space. */
			assert(trailsize < alloc_size);
			arena_run_trim_tail(arena, chunk, ret, size + trailsize,
			    size, false);
		}
	}

#ifdef JEMALLOC_STATS
	arena->stats.nmalloc_large++;
	arena->stats.nrequests_large++;
	arena->stats.allocated_large += size;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nmalloc++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nrequests++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns++;
	if (arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns >
	    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns) {
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns =
		    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns;
	}
#endif
	malloc_mutex_unlock(&arena->lock);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ret, 0xa5, size);
	else if (opt_zero)
		memset(ret, 0, size);
#endif
	return (ret);
}

/* Return the size of the allocation pointed to by ptr. */
size_t
arena_salloc(const void *ptr)
{
	size_t ret;
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapbits = chunk->map[pageind].bits;
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	if ((mapbits & CHUNK_MAP_LARGE) == 0) {
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)((pageind - (mapbits >> PAGE_SHIFT)) <<
		    PAGE_SHIFT));
		assert(run->magic == ARENA_RUN_MAGIC);
		assert(((uintptr_t)ptr - ((uintptr_t)run +
		    (uintptr_t)run->bin->reg0_offset)) % run->bin->reg_size ==
		    0);
		ret = run->bin->reg_size;
	} else {
		assert(((uintptr_t)ptr & PAGE_MASK) == 0);
		ret = mapbits & ~PAGE_MASK;
		assert(ret != 0);
	}

	return (ret);
}

#ifdef JEMALLOC_PROF
void
arena_prof_promoted(const void *ptr, size_t size)
{
	arena_chunk_t *chunk;
	size_t pageind, binind;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);
	assert(isalloc(ptr) == PAGE_SIZE);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	binind = small_size2bin[size];
	assert(binind < nbins);
	chunk->map[pageind].bits = (chunk->map[pageind].bits &
	    ~CHUNK_MAP_CLASS_MASK) | (binind << CHUNK_MAP_CLASS_SHIFT);
}

size_t
arena_salloc_demote(const void *ptr)
{
	size_t ret;
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapbits = chunk->map[pageind].bits;
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	if ((mapbits & CHUNK_MAP_LARGE) == 0) {
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)((pageind - (mapbits >> PAGE_SHIFT)) <<
		    PAGE_SHIFT));
		assert(run->magic == ARENA_RUN_MAGIC);
		assert(((uintptr_t)ptr - ((uintptr_t)run +
		    (uintptr_t)run->bin->reg0_offset)) % run->bin->reg_size ==
		    0);
		ret = run->bin->reg_size;
	} else {
		assert(((uintptr_t)ptr & PAGE_MASK) == 0);
		ret = mapbits & ~PAGE_MASK;
		if (prof_promote && ret == PAGE_SIZE && (mapbits &
		    CHUNK_MAP_CLASS_MASK) != CHUNK_MAP_CLASS_MASK) {
			size_t binind = ((mapbits & CHUNK_MAP_CLASS_MASK) >>
			    CHUNK_MAP_CLASS_SHIFT);
			assert(binind < nbins);
			ret = chunk->arena->bins[binind].reg_size;
		}
		assert(ret != 0);
	}

	return (ret);
}

static inline unsigned
arena_run_regind(arena_run_t *run, arena_bin_t *bin, const void *ptr,
    size_t size)
{
	unsigned shift, diff, regind;

	assert(run->magic == ARENA_RUN_MAGIC);

	/*
	 * Avoid doing division with a variable divisor if possible.  Using
	 * actual division here can reduce allocator throughput by over 20%!
	 */
	diff = (unsigned)((uintptr_t)ptr - (uintptr_t)run - bin->reg0_offset);

	/* Rescale (factor powers of 2 out of the numerator and denominator). */
	shift = ffs(size) - 1;
	diff >>= shift;
	size >>= shift;

	if (size == 1) {
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
		 *   (X * size_invs[D - 3]) >> SIZE_INV_SHIFT
		 *
		 * We can omit the first three elements, because we never
		 * divide by 0, and 1 and 2 are both powers of two, which are
		 * handled above.
		 */
#define	SIZE_INV_SHIFT 21
#define	SIZE_INV(s) (((1U << SIZE_INV_SHIFT) / (s)) + 1)
		static const unsigned size_invs[] = {
		    SIZE_INV(3),
		    SIZE_INV(4), SIZE_INV(5), SIZE_INV(6), SIZE_INV(7),
		    SIZE_INV(8), SIZE_INV(9), SIZE_INV(10), SIZE_INV(11),
		    SIZE_INV(12), SIZE_INV(13), SIZE_INV(14), SIZE_INV(15),
		    SIZE_INV(16), SIZE_INV(17), SIZE_INV(18), SIZE_INV(19),
		    SIZE_INV(20), SIZE_INV(21), SIZE_INV(22), SIZE_INV(23),
		    SIZE_INV(24), SIZE_INV(25), SIZE_INV(26), SIZE_INV(27),
		    SIZE_INV(28), SIZE_INV(29), SIZE_INV(30), SIZE_INV(31)
		};

		if (size <= ((sizeof(size_invs) / sizeof(unsigned)) + 2))
			regind = (diff * size_invs[size - 3]) >> SIZE_INV_SHIFT;
		else
			regind = diff / size;
#undef SIZE_INV
#undef SIZE_INV_SHIFT
	}
	assert(diff == regind * size);
	assert(regind < bin->nregs);

	return (regind);
}

prof_thr_cnt_t *
arena_prof_cnt_get(const void *ptr)
{
	prof_thr_cnt_t *ret;
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapbits = chunk->map[pageind].bits;
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	if ((mapbits & CHUNK_MAP_LARGE) == 0) {
		if (prof_promote)
			ret = (prof_thr_cnt_t *)(uintptr_t)1U;
		else {
			arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
			    (uintptr_t)((pageind - (mapbits >> PAGE_SHIFT)) <<
			    PAGE_SHIFT));
			arena_bin_t *bin = run->bin;
			unsigned regind;

			assert(run->magic == ARENA_RUN_MAGIC);
			regind = arena_run_regind(run, bin, ptr, bin->reg_size);
			ret = *(prof_thr_cnt_t **)((uintptr_t)run +
			    bin->cnt0_offset + (regind *
			    sizeof(prof_thr_cnt_t *)));
		}
	} else
		ret = chunk->map[pageind].prof_cnt;

	return (ret);
}

void
arena_prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt)
{
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapbits = chunk->map[pageind].bits;
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	if ((mapbits & CHUNK_MAP_LARGE) == 0) {
		if (prof_promote == false) {
			arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
			    (uintptr_t)((pageind - (mapbits >> PAGE_SHIFT)) <<
			    PAGE_SHIFT));
			arena_bin_t *bin = run->bin;
			unsigned regind;

			assert(run->magic == ARENA_RUN_MAGIC);
			regind = arena_run_regind(run, bin, ptr, bin->reg_size);

			*((prof_thr_cnt_t **)((uintptr_t)run + bin->cnt0_offset
			    + (regind * sizeof(prof_thr_cnt_t *)))) = cnt;
		} else
			assert((uintptr_t)cnt == (uintptr_t)1U);
	} else
		chunk->map[pageind].prof_cnt = cnt;
}
#endif

static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{
	size_t npages, run_ind, past;

	/* Dissociate run from bin. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else if (bin->nregs != 1) {
		size_t run_pageind = (((uintptr_t)run - (uintptr_t)chunk)) >>
		    PAGE_SHIFT;
		arena_chunk_map_t *run_mapelm = &chunk->map[run_pageind];
		/*
		 * This block's conditional is necessary because if the run
		 * only contains one region, then it never gets inserted into
		 * the non-full runs tree.
		 */
		arena_run_tree_remove(&bin->runs, run_mapelm);
	}

	malloc_mutex_unlock(&bin->lock);
	/******************************/
	npages = bin->run_size >> PAGE_SHIFT;
	run_ind = (size_t)(((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT);
	past = (size_t)(((uintptr_t)run->next - (uintptr_t)1U -
	    (uintptr_t)chunk) >> PAGE_SHIFT) + 1;
	malloc_mutex_lock(&arena->lock);

	/*
	 * If the run was originally clean, and some pages were never touched,
	 * trim the clean pages before deallocating the dirty portion of the
	 * run.
	 */
	if ((chunk->map[run_ind].bits & CHUNK_MAP_DIRTY) == 0 && past - run_ind
	    < npages) {
		/*
		 * Trim clean pages.  Convert to large run beforehand.  Set the
		 * last map element first, in case this is a one-page run.
		 */
		chunk->map[run_ind+npages-1].bits = CHUNK_MAP_LARGE |
		    (chunk->map[run_ind].bits & CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind].bits = bin->run_size | CHUNK_MAP_LARGE |
		    (chunk->map[run_ind].bits & CHUNK_MAP_FLAGS_MASK);
		arena_run_trim_tail(arena, chunk, run, (npages << PAGE_SHIFT),
		    ((npages - (past - run_ind)) << PAGE_SHIFT), false);
		npages = past - run_ind;
	}
#ifdef JEMALLOC_DEBUG
	run->magic = 0;
#endif
	arena_run_dalloc(arena, run, true);
	malloc_mutex_unlock(&arena->lock);
	/****************************/
	malloc_mutex_lock(&bin->lock);
#ifdef JEMALLOC_STATS
	bin->stats.curruns--;
#endif
}

void
arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_t *mapelm)
{
	size_t pageind;
	arena_run_t *run;
	arena_bin_t *bin;
#if (defined(JEMALLOC_FILL) || defined(JEMALLOC_STATS))
	size_t size;
#endif

	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    (mapelm->bits >> PAGE_SHIFT)) << PAGE_SHIFT));
	assert(run->magic == ARENA_RUN_MAGIC);
	bin = run->bin;
#if (defined(JEMALLOC_FILL) || defined(JEMALLOC_STATS))
	size = bin->reg_size;
#endif

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, size);
#endif

	arena_run_reg_dalloc(run, ptr);

	if (run->nfree == bin->nregs)
		arena_dalloc_bin_run(arena, chunk, run, bin);
	else if (run->nfree == 1 && run != bin->runcur) {
		/*
		 * Make sure that bin->runcur always refers to the lowest
		 * non-full run, if one exists.
		 */
		if (bin->runcur == NULL)
			bin->runcur = run;
		else if ((uintptr_t)run < (uintptr_t)bin->runcur) {
			/* Switch runcur. */
			if (bin->runcur->nfree > 0) {
				arena_chunk_t *runcur_chunk =
				    CHUNK_ADDR2BASE(bin->runcur);
				size_t runcur_pageind =
				    (((uintptr_t)bin->runcur -
				    (uintptr_t)runcur_chunk)) >> PAGE_SHIFT;
				arena_chunk_map_t *runcur_mapelm =
				    &runcur_chunk->map[runcur_pageind];

				/* Insert runcur. */
				arena_run_tree_insert(&bin->runs,
				    runcur_mapelm);
			}
			bin->runcur = run;
		} else {
			size_t run_pageind = (((uintptr_t)run -
			    (uintptr_t)chunk)) >> PAGE_SHIFT;
			arena_chunk_map_t *run_mapelm =
			    &chunk->map[run_pageind];

			assert(arena_run_tree_search(&bin->runs, run_mapelm) ==
			    NULL);
			arena_run_tree_insert(&bin->runs, run_mapelm);
		}
	}

#ifdef JEMALLOC_STATS
	bin->stats.allocated -= size;
	bin->stats.ndalloc++;
#endif
}

#ifdef JEMALLOC_STATS
void
arena_stats_merge(arena_t *arena, size_t *nactive, size_t *ndirty,
    arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats)
{
	unsigned i;

	malloc_mutex_lock(&arena->lock);
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

	for (i = 0; i < nlclasses; i++) {
		lstats[i].nmalloc += arena->stats.lstats[i].nmalloc;
		lstats[i].ndalloc += arena->stats.lstats[i].ndalloc;
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].highruns += arena->stats.lstats[i].highruns;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}
	malloc_mutex_unlock(&arena->lock);

	for (i = 0; i < nbins; i++) {
		arena_bin_t *bin = &arena->bins[i];

		malloc_mutex_lock(&bin->lock);
		bstats[i].allocated += bin->stats.allocated;
		bstats[i].nmalloc += bin->stats.nmalloc;
		bstats[i].ndalloc += bin->stats.ndalloc;
		bstats[i].nrequests += bin->stats.nrequests;
#ifdef JEMALLOC_TCACHE
		bstats[i].nfills += bin->stats.nfills;
		bstats[i].nflushes += bin->stats.nflushes;
#endif
		bstats[i].nruns += bin->stats.nruns;
		bstats[i].reruns += bin->stats.reruns;
		bstats[i].highruns += bin->stats.highruns;
		bstats[i].curruns += bin->stats.curruns;
		malloc_mutex_unlock(&bin->lock);
	}
}
#endif

void
arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{

	/* Large allocation. */
#ifdef JEMALLOC_FILL
#  ifndef JEMALLOC_STATS
	if (opt_junk)
#  endif
#endif
	{
#if (defined(JEMALLOC_FILL) || defined(JEMALLOC_STATS))
		size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >>
		    PAGE_SHIFT;
		size_t size = chunk->map[pageind].bits & ~PAGE_MASK;
#endif

#ifdef JEMALLOC_FILL
#  ifdef JEMALLOC_STATS
		if (opt_junk)
#  endif
			memset(ptr, 0x5a, size);
#endif
#ifdef JEMALLOC_STATS
		arena->stats.ndalloc_large++;
		arena->stats.allocated_large -= size;
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].ndalloc++;
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns--;
#endif
	}

	arena_run_dalloc(arena, (arena_run_t *)ptr, true);
}

static void
arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t size, size_t oldsize)
{

	assert(size < oldsize);

	/*
	 * Shrink the run, and make trailing pages available for other
	 * allocations.
	 */
	malloc_mutex_lock(&arena->lock);
	arena_run_trim_tail(arena, chunk, (arena_run_t *)ptr, oldsize, size,
	    true);
#ifdef JEMALLOC_STATS
	arena->stats.ndalloc_large++;
	arena->stats.allocated_large -= oldsize;
	arena->stats.lstats[(oldsize >> PAGE_SHIFT) - 1].ndalloc++;
	arena->stats.lstats[(oldsize >> PAGE_SHIFT) - 1].curruns--;

	arena->stats.nmalloc_large++;
	arena->stats.nrequests_large++;
	arena->stats.allocated_large += size;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nmalloc++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nrequests++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns++;
	if (arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns >
	    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns) {
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns =
		    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns;
	}
#endif
	malloc_mutex_unlock(&arena->lock);
}

static bool
arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t size, size_t oldsize)
{
	size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT;
	size_t npages = oldsize >> PAGE_SHIFT;

	assert(oldsize == (chunk->map[pageind].bits & ~PAGE_MASK));

	/* Try to extend the run. */
	assert(size > oldsize);
	malloc_mutex_lock(&arena->lock);
	if (pageind + npages < chunk_npages && (chunk->map[pageind+npages].bits
	    & CHUNK_MAP_ALLOCATED) == 0 && (chunk->map[pageind+npages].bits &
	    ~PAGE_MASK) >= size - oldsize) {
		/*
		 * The next run is available and sufficiently large.  Split the
		 * following run, then merge the first part with the existing
		 * allocation.
		 */
		arena_run_split(arena, (arena_run_t *)((uintptr_t)chunk +
		    ((pageind+npages) << PAGE_SHIFT)), size - oldsize, true,
		    false);

		chunk->map[pageind].bits = size | CHUNK_MAP_LARGE |
		    CHUNK_MAP_ALLOCATED;
		chunk->map[pageind+npages].bits = CHUNK_MAP_LARGE |
		    CHUNK_MAP_ALLOCATED;

#ifdef JEMALLOC_STATS
	arena->stats.ndalloc_large++;
	arena->stats.allocated_large -= oldsize;
	arena->stats.lstats[(oldsize >> PAGE_SHIFT) - 1].ndalloc++;
	arena->stats.lstats[(oldsize >> PAGE_SHIFT) - 1].curruns--;

	arena->stats.nmalloc_large++;
	arena->stats.nrequests_large++;
	arena->stats.allocated_large += size;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nmalloc++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nrequests++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns++;
	if (arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns >
	    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns) {
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns =
		    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns;
	}
#endif
		malloc_mutex_unlock(&arena->lock);
		return (false);
	}
	malloc_mutex_unlock(&arena->lock);

	return (true);
}

/*
 * Try to resize a large allocation, in order to avoid copying.  This will
 * always fail if growing an object, and the following run is already in use.
 */
static bool
arena_ralloc_large(void *ptr, size_t size, size_t oldsize)
{
	size_t psize;

	psize = PAGE_CEILING(size);
	if (psize == oldsize) {
		/* Same size class. */
#ifdef JEMALLOC_FILL
		if (opt_junk && size < oldsize) {
			memset((void *)((uintptr_t)ptr + size), 0x5a, oldsize -
			    size);
		}
#endif
		return (false);
	} else {
		arena_chunk_t *chunk;
		arena_t *arena;

		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
		arena = chunk->arena;
		assert(arena->magic == ARENA_MAGIC);

		if (psize < oldsize) {
#ifdef JEMALLOC_FILL
			/* Fill before shrinking in order avoid a race. */
			if (opt_junk) {
				memset((void *)((uintptr_t)ptr + size), 0x5a,
				    oldsize - size);
			}
#endif
			arena_ralloc_large_shrink(arena, chunk, ptr, psize,
			    oldsize);
			return (false);
		} else {
			bool ret = arena_ralloc_large_grow(arena, chunk, ptr,
			    psize, oldsize);
#ifdef JEMALLOC_FILL
			if (ret == false && opt_zero) {
				memset((void *)((uintptr_t)ptr + oldsize), 0,
				    size - oldsize);
			}
#endif
			return (ret);
		}
	}
}

void *
arena_ralloc(void *ptr, size_t size, size_t oldsize)
{
	void *ret;
	size_t copysize;

	/* Try to avoid moving the allocation. */
	if (oldsize <= arena_maxclass) {
		if (oldsize <= small_maxclass) {
			if (size <= small_maxclass && small_size2bin[size] ==
			    small_size2bin[oldsize])
				goto IN_PLACE;
		} else {
			assert(size <= arena_maxclass);
			if (size > small_maxclass) {
				if (arena_ralloc_large(ptr, size, oldsize) ==
				    false)
					return (ptr);
			}
		}
	}

	/*
	 * If we get here, then size and oldsize are different enough that we
	 * need to move the object.  In that case, fall back to allocating new
	 * space and copying.
	 */
	ret = arena_malloc(size, false);
	if (ret == NULL)
		return (NULL);

	/* Junk/zero-filling were already done by arena_malloc(). */
	copysize = (size < oldsize) ? size : oldsize;
	memcpy(ret, ptr, copysize);
	idalloc(ptr);
	return (ret);
IN_PLACE:
#ifdef JEMALLOC_FILL
	if (opt_junk && size < oldsize)
		memset((void *)((uintptr_t)ptr + size), 0x5a, oldsize - size);
	else if (opt_zero && size > oldsize)
		memset((void *)((uintptr_t)ptr + oldsize), 0, size - oldsize);
#endif
	return (ptr);
}

bool
arena_new(arena_t *arena, unsigned ind)
{
	unsigned i;
	arena_bin_t *bin;
	size_t prev_run_size;

	arena->ind = ind;

	if (malloc_mutex_init(&arena->lock))
		return (true);

#ifdef JEMALLOC_STATS
	memset(&arena->stats, 0, sizeof(arena_stats_t));
	arena->stats.lstats = (malloc_large_stats_t *)base_alloc(nlclasses *
	    sizeof(malloc_large_stats_t));
	if (arena->stats.lstats == NULL)
		return (true);
	memset(arena->stats.lstats, 0, nlclasses *
	    sizeof(malloc_large_stats_t));
#  ifdef JEMALLOC_TCACHE
	ql_new(&arena->tcache_ql);
#  endif
#endif

#ifdef JEMALLOC_PROF
	arena->prof_accumbytes = 0;
#endif

	/* Initialize chunks. */
	ql_new(&arena->chunks_dirty);
	arena->spare = NULL;

	arena->nactive = 0;
	arena->ndirty = 0;
	arena->npurgatory = 0;

	arena_avail_tree_new(&arena->runs_avail_clean);
	arena_avail_tree_new(&arena->runs_avail_dirty);

	/* Initialize bins. */
	prev_run_size = PAGE_SIZE;

	i = 0;
#ifdef JEMALLOC_TINY
	/* (2^n)-spaced tiny bins. */
	for (; i < ntbins; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = (1U << (LG_TINY_MIN + i));

		prev_run_size = arena_bin_run_size_calc(bin, prev_run_size);

#ifdef JEMALLOC_STATS
		memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
#endif
	}
#endif

	/* Quantum-spaced bins. */
	for (; i < ntbins + nqbins; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = (i - ntbins + 1) << LG_QUANTUM;

		prev_run_size = arena_bin_run_size_calc(bin, prev_run_size);

#ifdef JEMALLOC_STATS
		memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
#endif
	}

	/* Cacheline-spaced bins. */
	for (; i < ntbins + nqbins + ncbins; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = cspace_min + ((i - (ntbins + nqbins)) <<
		    LG_CACHELINE);

		prev_run_size = arena_bin_run_size_calc(bin, prev_run_size);

#ifdef JEMALLOC_STATS
		memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
#endif
	}

	/* Subpage-spaced bins. */
	for (; i < nbins; i++) {
		bin = &arena->bins[i];
		if (malloc_mutex_init(&bin->lock))
			return (true);
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = sspace_min + ((i - (ntbins + nqbins + ncbins))
		    << LG_SUBPAGE);

		prev_run_size = arena_bin_run_size_calc(bin, prev_run_size);

#ifdef JEMALLOC_STATS
		memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
#endif
	}

#ifdef JEMALLOC_DEBUG
	arena->magic = ARENA_MAGIC;
#endif

	return (false);
}

#ifdef JEMALLOC_TINY
/* Compute the smallest power of 2 that is >= x. */
static size_t
pow2_ceil(size_t x)
{

	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
#if (SIZEOF_PTR == 8)
	x |= x >> 32;
#endif
	x++;
	return (x);
}
#endif

#ifdef JEMALLOC_DEBUG
static void
small_size2bin_validate(void)
{
	size_t i, size, binind;

	assert(small_size2bin[0] == 0xffU);
	i = 1;
#  ifdef JEMALLOC_TINY
	/* Tiny. */
	for (; i < (1U << LG_TINY_MIN); i++) {
		size = pow2_ceil(1U << LG_TINY_MIN);
		binind = ffs((int)(size >> (LG_TINY_MIN + 1)));
		assert(small_size2bin[i] == binind);
	}
	for (; i < qspace_min; i++) {
		size = pow2_ceil(i);
		binind = ffs((int)(size >> (LG_TINY_MIN + 1)));
		assert(small_size2bin[i] == binind);
	}
#  endif
	/* Quantum-spaced. */
	for (; i <= qspace_max; i++) {
		size = QUANTUM_CEILING(i);
		binind = ntbins + (size >> LG_QUANTUM) - 1;
		assert(small_size2bin[i] == binind);
	}
	/* Cacheline-spaced. */
	for (; i <= cspace_max; i++) {
		size = CACHELINE_CEILING(i);
		binind = ntbins + nqbins + ((size - cspace_min) >>
		    LG_CACHELINE);
		assert(small_size2bin[i] == binind);
	}
	/* Sub-page. */
	for (; i <= sspace_max; i++) {
		size = SUBPAGE_CEILING(i);
		binind = ntbins + nqbins + ncbins + ((size - sspace_min)
		    >> LG_SUBPAGE);
		assert(small_size2bin[i] == binind);
	}
}
#endif

static bool
small_size2bin_init(void)
{

	if (opt_lg_qspace_max != LG_QSPACE_MAX_DEFAULT
	    || opt_lg_cspace_max != LG_CSPACE_MAX_DEFAULT
	    || sizeof(const_small_size2bin) != small_maxclass + 1)
		return (small_size2bin_init_hard());

	small_size2bin = const_small_size2bin;
#ifdef JEMALLOC_DEBUG
	assert(sizeof(const_small_size2bin) == small_maxclass + 1);
	small_size2bin_validate();
#endif
	return (false);
}

static bool
small_size2bin_init_hard(void)
{
	size_t i, size, binind;
	uint8_t *custom_small_size2bin;

	assert(opt_lg_qspace_max != LG_QSPACE_MAX_DEFAULT
	    || opt_lg_cspace_max != LG_CSPACE_MAX_DEFAULT
	    || sizeof(const_small_size2bin) != small_maxclass + 1);

	custom_small_size2bin = (uint8_t *)base_alloc(small_maxclass + 1);
	if (custom_small_size2bin == NULL)
		return (true);

	custom_small_size2bin[0] = 0xffU;
	i = 1;
#ifdef JEMALLOC_TINY
	/* Tiny. */
	for (; i < (1U << LG_TINY_MIN); i++) {
		size = pow2_ceil(1U << LG_TINY_MIN);
		binind = ffs((int)(size >> (LG_TINY_MIN + 1)));
		custom_small_size2bin[i] = binind;
	}
	for (; i < qspace_min; i++) {
		size = pow2_ceil(i);
		binind = ffs((int)(size >> (LG_TINY_MIN + 1)));
		custom_small_size2bin[i] = binind;
	}
#endif
	/* Quantum-spaced. */
	for (; i <= qspace_max; i++) {
		size = QUANTUM_CEILING(i);
		binind = ntbins + (size >> LG_QUANTUM) - 1;
		custom_small_size2bin[i] = binind;
	}
	/* Cacheline-spaced. */
	for (; i <= cspace_max; i++) {
		size = CACHELINE_CEILING(i);
		binind = ntbins + nqbins + ((size - cspace_min) >>
		    LG_CACHELINE);
		custom_small_size2bin[i] = binind;
	}
	/* Sub-page. */
	for (; i <= sspace_max; i++) {
		size = SUBPAGE_CEILING(i);
		binind = ntbins + nqbins + ncbins + ((size - sspace_min) >>
		    LG_SUBPAGE);
		custom_small_size2bin[i] = binind;
	}

	small_size2bin = custom_small_size2bin;
#ifdef JEMALLOC_DEBUG
	small_size2bin_validate();
#endif
	return (false);
}

bool
arena_boot(void)
{
	size_t header_size;

	/* Set variables according to the value of opt_lg_[qc]space_max. */
	qspace_max = (1U << opt_lg_qspace_max);
	cspace_min = CACHELINE_CEILING(qspace_max);
	if (cspace_min == qspace_max)
		cspace_min += CACHELINE;
	cspace_max = (1U << opt_lg_cspace_max);
	sspace_min = SUBPAGE_CEILING(cspace_max);
	if (sspace_min == cspace_max)
		sspace_min += SUBPAGE;
	assert(sspace_min < PAGE_SIZE);
	sspace_max = PAGE_SIZE - SUBPAGE;

#ifdef JEMALLOC_TINY
	assert(LG_QUANTUM >= LG_TINY_MIN);
#endif
	assert(ntbins <= LG_QUANTUM);
	nqbins = qspace_max >> LG_QUANTUM;
	ncbins = ((cspace_max - cspace_min) >> LG_CACHELINE) + 1;
	nsbins = ((sspace_max - sspace_min) >> LG_SUBPAGE) + 1;
	nbins = ntbins + nqbins + ncbins + nsbins;

	/*
	 * The small_size2bin lookup table uses uint8_t to encode each bin
	 * index, so we cannot support more than 256 small size classes.  This
	 * limit is difficult to exceed (not even possible with 16B quantum and
	 * 4KiB pages), and such configurations are impractical, but
	 * nonetheless we need to protect against this case in order to avoid
	 * undefined behavior.
	 *
	 * Further constrain nbins to 255 if prof_promote is true, since all
	 * small size classes, plus a "not small" size class must be stored in
	 * 8 bits of arena_chunk_map_t's bits field.
	 */
#ifdef JEMALLOC_PROF
	if (opt_prof && prof_promote) {
		if (nbins > 255) {
		    char line_buf[UMAX2S_BUFSIZE];
		    malloc_write("<jemalloc>: Too many small size classes (");
		    malloc_write(umax2s(nbins, 10, line_buf));
		    malloc_write(" > max 255)\n");
		    abort();
		}
	} else
#endif
	if (nbins > 256) {
	    char line_buf[UMAX2S_BUFSIZE];
	    malloc_write("<jemalloc>: Too many small size classes (");
	    malloc_write(umax2s(nbins, 10, line_buf));
	    malloc_write(" > max 256)\n");
	    abort();
	}

	if (small_size2bin_init())
		return (true);

	/*
	 * Compute the header size such that it is large enough to contain the
	 * page map.
	 */
	header_size = sizeof(arena_chunk_t) +
	    (sizeof(arena_chunk_map_t) * (chunk_npages - 1));
	arena_chunk_header_npages = (header_size >> PAGE_SHIFT) +
	    ((header_size & PAGE_MASK) != 0);
	arena_maxclass = chunksize - (arena_chunk_header_npages << PAGE_SHIFT);

	return (false);
}
