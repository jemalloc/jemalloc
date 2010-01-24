#define	JEMALLOC_ARENA_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

size_t	opt_lg_qspace_max = LG_QSPACE_MAX_DEFAULT;
size_t	opt_lg_cspace_max = LG_CSPACE_MAX_DEFAULT;
size_t	opt_lg_medium_max = LG_MEDIUM_MAX_DEFAULT;
ssize_t		opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
uint8_t const	*small_size2bin;

/* Various bin-related settings. */
unsigned	nqbins;
unsigned	ncbins;
unsigned	nsbins;
unsigned	nmbins;
unsigned	nbins;
unsigned	mbin0;
size_t		qspace_max;
size_t		cspace_min;
size_t		cspace_max;
size_t		sspace_min;
size_t		sspace_max;
size_t		medium_max;

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
/* 64-bit system ************************/
#  ifdef JEMALLOC_TINY
	S2B_2(0)		/*    2 */
	S2B_2(1)		/*    4 */
	S2B_4(2)		/*    8 */
	S2B_8(3)		/*   16 */
#    define S2B_QMIN 3
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
/* 32-bit system ************************/
#  ifdef JEMALLOC_TINY
	S2B_2(0)		/*    2 */
	S2B_2(1)		/*    4 */
	S2B_4(2)		/*    8 */
#    define S2B_QMIN 2
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
static void	*arena_malloc_large(arena_t *arena, size_t size, bool zero);
static bool	arena_is_large(const void *ptr);
static void	arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk,
    arena_run_t *run, arena_bin_t *bin);
#ifdef JEMALLOC_STATS
static void	arena_stats_aprint(size_t nactive, size_t ndirty,
    const arena_stats_t *astats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque);
static void	arena_stats_bprint(arena_t *arena,
    const malloc_bin_stats_t *bstats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque);
static void	arena_stats_lprint(const malloc_large_stats_t *lstats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque);
#endif
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
arena_chunk_comp(arena_chunk_t *a, arena_chunk_t *b)
{
	uintptr_t a_chunk = (uintptr_t)a;
	uintptr_t b_chunk = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_chunk > b_chunk) - (a_chunk < b_chunk));
}

/* Wrap red-black tree macros in functions. */
rb_wrap(static JEMALLOC_ATTR(unused), arena_chunk_tree_dirty_,
    arena_chunk_tree_t, arena_chunk_t, link_dirty, arena_chunk_comp)

static inline int
arena_run_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	uintptr_t a_mapelm = (uintptr_t)a;
	uintptr_t b_mapelm = (uintptr_t)b;

	assert(a != NULL);
	assert(b != NULL);

	return ((a_mapelm > b_mapelm) - (a_mapelm < b_mapelm));
}

/* Wrap red-black tree macros in functions. */
rb_wrap(static JEMALLOC_ATTR(unused), arena_run_tree_, arena_run_tree_t,
    arena_chunk_map_t, link, arena_run_comp)

static inline int
arena_avail_comp(arena_chunk_map_t *a, arena_chunk_map_t *b)
{
	int ret;
	size_t a_size = a->bits & ~PAGE_MASK;
	size_t b_size = b->bits & ~PAGE_MASK;

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

/* Wrap red-black tree macros in functions. */
rb_wrap(static JEMALLOC_ATTR(unused), arena_avail_tree_, arena_avail_tree_t,
    arena_chunk_map_t, link, arena_avail_comp)

static inline void
arena_run_rc_incr(arena_run_t *run, arena_bin_t *bin, const void *ptr)
{
	arena_chunk_t *chunk;
	arena_t *arena;
	size_t pagebeg, pageend, i;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pagebeg = ((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT;
	pageend = ((uintptr_t)ptr + (uintptr_t)(bin->reg_size - 1) -
	    (uintptr_t)chunk) >> PAGE_SHIFT;

	for (i = pagebeg; i <= pageend; i++) {
		size_t mapbits = chunk->map[i].bits;

		if (mapbits & CHUNK_MAP_DIRTY) {
			assert((mapbits & CHUNK_MAP_RC_MASK) == 0);
			chunk->ndirty--;
			arena->ndirty--;
			mapbits ^= CHUNK_MAP_DIRTY;
		}
		assert((mapbits & CHUNK_MAP_RC_MASK) != CHUNK_MAP_RC_MASK);
		mapbits += CHUNK_MAP_RC_ONE;
		chunk->map[i].bits = mapbits;
	}
}

static inline void
arena_run_rc_decr(arena_run_t *run, arena_bin_t *bin, const void *ptr)
{
	arena_chunk_t *chunk;
	arena_t *arena;
	size_t pagebeg, pageend, mapbits, i;
	bool dirtier = false;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pagebeg = ((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT;
	pageend = ((uintptr_t)ptr + (uintptr_t)(bin->reg_size - 1) -
	    (uintptr_t)chunk) >> PAGE_SHIFT;

	/* First page. */
	mapbits = chunk->map[pagebeg].bits;
	mapbits -= CHUNK_MAP_RC_ONE;
	if ((mapbits & CHUNK_MAP_RC_MASK) == 0) {
		dirtier = true;
		assert((mapbits & CHUNK_MAP_DIRTY) == 0);
		mapbits |= CHUNK_MAP_DIRTY;
		chunk->ndirty++;
		arena->ndirty++;
	}
	chunk->map[pagebeg].bits = mapbits;

	if (pageend - pagebeg >= 1) {
		/*
		 * Interior pages are completely consumed by the object being
		 * deallocated, which means that the pages can be
		 * unconditionally marked dirty.
		 */
		for (i = pagebeg + 1; i < pageend; i++) {
			mapbits = chunk->map[i].bits;
			mapbits -= CHUNK_MAP_RC_ONE;
			assert((mapbits & CHUNK_MAP_RC_MASK) == 0);
			dirtier = true;
			assert((mapbits & CHUNK_MAP_DIRTY) == 0);
			mapbits |= CHUNK_MAP_DIRTY;
			chunk->ndirty++;
			arena->ndirty++;
			chunk->map[i].bits = mapbits;
		}

		/* Last page. */
		mapbits = chunk->map[pageend].bits;
		mapbits -= CHUNK_MAP_RC_ONE;
		if ((mapbits & CHUNK_MAP_RC_MASK) == 0) {
			dirtier = true;
			assert((mapbits & CHUNK_MAP_DIRTY) == 0);
			mapbits |= CHUNK_MAP_DIRTY;
			chunk->ndirty++;
			arena->ndirty++;
		}
		chunk->map[pageend].bits = mapbits;
	}

	if (dirtier) {
		if (chunk->dirtied == false) {
			arena_chunk_tree_dirty_insert(&arena->chunks_dirty,
			    chunk);
			chunk->dirtied = true;
		}

		/* Enforce opt_lg_dirty_mult. */
		if (opt_lg_dirty_mult >= 0 && (arena->nactive >>
		    opt_lg_dirty_mult) < arena->ndirty)
			arena_purge(arena);
	}
}

static inline void *
arena_run_reg_alloc(arena_run_t *run, arena_bin_t *bin)
{
	void *ret;
	unsigned i, mask, bit, regind;

	assert(run->magic == ARENA_RUN_MAGIC);
	assert(run->regs_minelm < bin->regs_mask_nelms);

	/*
	 * Move the first check outside the loop, so that run->regs_minelm can
	 * be updated unconditionally, without the possibility of updating it
	 * multiple times.
	 */
	i = run->regs_minelm;
	mask = run->regs_mask[i];
	if (mask != 0) {
		/* Usable allocation found. */
		bit = ffs((int)mask) - 1;

		regind = ((i << (LG_SIZEOF_INT + 3)) + bit);
		assert(regind < bin->nregs);
		ret = (void *)(((uintptr_t)run) + bin->reg0_offset
		    + (bin->reg_size * regind));

		/* Clear bit. */
		mask ^= (1U << bit);
		run->regs_mask[i] = mask;

		arena_run_rc_incr(run, bin, ret);

		return (ret);
	}

	for (i++; i < bin->regs_mask_nelms; i++) {
		mask = run->regs_mask[i];
		if (mask != 0) {
			/* Usable allocation found. */
			bit = ffs((int)mask) - 1;

			regind = ((i << (LG_SIZEOF_INT + 3)) + bit);
			assert(regind < bin->nregs);
			ret = (void *)(((uintptr_t)run) + bin->reg0_offset
			    + (bin->reg_size * regind));

			/* Clear bit. */
			mask ^= (1U << bit);
			run->regs_mask[i] = mask;

			/*
			 * Make a note that nothing before this element
			 * contains a free region.
			 */
			run->regs_minelm = i; /* Low payoff: + (mask == 0); */

			arena_run_rc_incr(run, bin, ret);

			return (ret);
		}
	}
	/* Not reached. */
	assert(0);
	return (NULL);
}

static inline void
arena_run_reg_dalloc(arena_run_t *run, arena_bin_t *bin, void *ptr, size_t size)
{
	unsigned shift, diff, regind, elm, bit;

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

	elm = regind >> (LG_SIZEOF_INT + 3);
	if (elm < run->regs_minelm)
		run->regs_minelm = elm;
	bit = regind - (elm << (LG_SIZEOF_INT + 3));
	assert((run->regs_mask[elm] & (1U << bit)) == 0);
	run->regs_mask[elm] |= (1U << bit);

	arena_run_rc_decr(run, bin, ptr);
}

static void
arena_run_split(arena_t *arena, arena_run_t *run, size_t size, bool large,
    bool zero)
{
	arena_chunk_t *chunk;
	size_t old_ndirty, run_ind, total_pages, need_pages, rem_pages, i;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(run);
	old_ndirty = chunk->ndirty;
	run_ind = (unsigned)(((uintptr_t)run - (uintptr_t)chunk)
	    >> PAGE_SHIFT);
	total_pages = (chunk->map[run_ind].bits & ~PAGE_MASK) >>
	    PAGE_SHIFT;
	need_pages = (size >> PAGE_SHIFT);
	assert(need_pages > 0);
	assert(need_pages <= total_pages);
	rem_pages = total_pages - need_pages;

	arena_avail_tree_remove(&arena->runs_avail, &chunk->map[run_ind]);
	arena->nactive += need_pages;

	/* Keep track of trailing unused pages for later use. */
	if (rem_pages > 0) {
		chunk->map[run_ind+need_pages].bits = (rem_pages <<
		    PAGE_SHIFT) | (chunk->map[run_ind+need_pages].bits &
		    CHUNK_MAP_FLAGS_MASK);
		chunk->map[run_ind+total_pages-1].bits = (rem_pages <<
		    PAGE_SHIFT) | (chunk->map[run_ind+total_pages-1].bits &
		    CHUNK_MAP_FLAGS_MASK);
		arena_avail_tree_insert(&arena->runs_avail,
		    &chunk->map[run_ind+need_pages]);
	}

	for (i = 0; i < need_pages; i++) {
		/* Zero if necessary. */
		if (zero) {
			if ((chunk->map[run_ind + i].bits & CHUNK_MAP_ZEROED)
			    == 0) {
				memset((void *)((uintptr_t)chunk + ((run_ind
				    + i) << PAGE_SHIFT)), 0, PAGE_SIZE);
				/* CHUNK_MAP_ZEROED is cleared below. */
			}
		}

		/* Update dirty page accounting. */
		if (chunk->map[run_ind + i].bits & CHUNK_MAP_DIRTY) {
			chunk->ndirty--;
			arena->ndirty--;
			/* CHUNK_MAP_DIRTY is cleared below. */
		}

		/* Initialize the chunk map. */
		if (large) {
			chunk->map[run_ind + i].bits = CHUNK_MAP_LARGE
			    | CHUNK_MAP_ALLOCATED;
		} else {
			chunk->map[run_ind + i].bits = (i << CHUNK_MAP_PG_SHIFT)
			    | CHUNK_MAP_ALLOCATED;
		}
	}

	if (large) {
		/*
		 * Set the run size only in the first element for large runs.
		 * This is primarily a debugging aid, since the lack of size
		 * info for trailing pages only matters if the application
		 * tries to operate on an interior pointer.
		 */
		chunk->map[run_ind].bits |= size;
	} else {
		/*
		 * Initialize the first page's refcount to 1, so that the run
		 * header is protected from dirty page purging.
		 */
		chunk->map[run_ind].bits += CHUNK_MAP_RC_ONE;
	}
}

static arena_chunk_t *
arena_chunk_alloc(arena_t *arena)
{
	arena_chunk_t *chunk;
	size_t i;

	if (arena->spare != NULL) {
		chunk = arena->spare;
		arena->spare = NULL;
	} else {
		chunk = (arena_chunk_t *)chunk_alloc(chunksize, true);
		if (chunk == NULL)
			return (NULL);
#ifdef JEMALLOC_STATS
		arena->stats.mapped += chunksize;
#endif

		chunk->arena = arena;
		chunk->dirtied = false;

		/*
		 * Claim that no pages are in use, since the header is merely
		 * overhead.
		 */
		chunk->ndirty = 0;

		/*
		 * Initialize the map to contain one maximal free untouched run.
		 */
		for (i = 0; i < arena_chunk_header_npages; i++)
			chunk->map[i].bits = 0;
		chunk->map[i].bits = arena_maxclass | CHUNK_MAP_ZEROED;
		for (i++; i < chunk_npages-1; i++) {
			chunk->map[i].bits = CHUNK_MAP_ZEROED;
		}
		chunk->map[chunk_npages-1].bits = arena_maxclass |
		    CHUNK_MAP_ZEROED;
	}

	/* Insert the run into the runs_avail tree. */
	arena_avail_tree_insert(&arena->runs_avail,
	    &chunk->map[arena_chunk_header_npages]);

	return (chunk);
}

static void
arena_chunk_dealloc(arena_t *arena, arena_chunk_t *chunk)
{

	if (arena->spare != NULL) {
		if (arena->spare->dirtied) {
			arena_chunk_tree_dirty_remove(
			    &chunk->arena->chunks_dirty, arena->spare);
			arena->ndirty -= arena->spare->ndirty;
		}
		chunk_dealloc((void *)arena->spare, chunksize);
#ifdef JEMALLOC_STATS
		arena->stats.mapped -= chunksize;
#endif
	}

	/*
	 * Remove run from runs_avail, regardless of whether this chunk
	 * will be cached, so that the arena does not use it.  Dirty page
	 * flushing only uses the chunks_dirty tree, so leaving this chunk in
	 * the chunks_* trees is sufficient for that purpose.
	 */
	arena_avail_tree_remove(&arena->runs_avail,
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
	mapelm = arena_avail_tree_nsearch(&arena->runs_avail, &key);
	if (mapelm != NULL) {
		arena_chunk_t *run_chunk = CHUNK_ADDR2BASE(mapelm);
		size_t pageind = ((uintptr_t)mapelm - (uintptr_t)run_chunk->map)
		    / sizeof(arena_chunk_map_t);

		run = (arena_run_t *)((uintptr_t)run_chunk + (pageind
		    << PAGE_SHIFT));
		arena_run_split(arena, run, size, large, zero);
		return (run);
	}

	/*
	 * No usable runs.  Create a new chunk from which to allocate the run.
	 */
	chunk = arena_chunk_alloc(arena);
	if (chunk == NULL)
		return (NULL);
	run = (arena_run_t *)((uintptr_t)chunk + (arena_chunk_header_npages <<
	    PAGE_SHIFT));
	/* Update page map. */
	arena_run_split(arena, run, size, large, zero);
	return (run);
}

static void
arena_purge(arena_t *arena)
{
	arena_chunk_t *chunk;
	size_t i, npages;
#ifdef JEMALLOC_DEBUG
	size_t ndirty = 0;

	rb_foreach_begin(arena_chunk_t, link_dirty, &arena->chunks_dirty,
	    chunk) {
		assert(chunk->dirtied);
		ndirty += chunk->ndirty;
	} rb_foreach_end(arena_chunk_t, link_dirty, &arena->chunks_dirty, chunk)
	assert(ndirty == arena->ndirty);
#endif
	assert((arena->nactive >> opt_lg_dirty_mult) < arena->ndirty);

#ifdef JEMALLOC_STATS
	arena->stats.npurge++;
#endif

	/*
	 * Iterate downward through chunks until enough dirty memory has been
	 * purged.  Terminate as soon as possible in order to minimize the
	 * number of system calls, even if a chunk has only been partially
	 * purged.
	 */

	while ((arena->nactive >> (opt_lg_dirty_mult + 1)) < arena->ndirty) {
		chunk = arena_chunk_tree_dirty_last(&arena->chunks_dirty);
		assert(chunk != NULL);

		for (i = chunk_npages - 1; chunk->ndirty > 0; i--) {
			assert(i >= arena_chunk_header_npages);
			if (chunk->map[i].bits & CHUNK_MAP_DIRTY) {
				chunk->map[i].bits ^= CHUNK_MAP_DIRTY;
				/* Find adjacent dirty run(s). */
				for (npages = 1; i > arena_chunk_header_npages
				    && (chunk->map[i - 1].bits &
				    CHUNK_MAP_DIRTY); npages++) {
					i--;
					chunk->map[i].bits ^= CHUNK_MAP_DIRTY;
				}
				chunk->ndirty -= npages;
				arena->ndirty -= npages;

				madvise((void *)((uintptr_t)chunk + (i <<
				    PAGE_SHIFT)), (npages << PAGE_SHIFT),
				    MADV_DONTNEED);
#ifdef JEMALLOC_STATS
				arena->stats.nmadvise++;
				arena->stats.purged += npages;
#endif
				if ((arena->nactive >> (opt_lg_dirty_mult + 1))
				    >= arena->ndirty)
					break;
			}
		}

		if (chunk->ndirty == 0) {
			arena_chunk_tree_dirty_remove(&arena->chunks_dirty,
			    chunk);
			chunk->dirtied = false;
		}
	}
}

static void
arena_run_dalloc(arena_t *arena, arena_run_t *run, bool dirty)
{
	arena_chunk_t *chunk;
	size_t size, run_ind, run_pages;

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

	/* Mark pages as unallocated in the chunk map. */
	if (dirty) {
		size_t i;

		for (i = 0; i < run_pages; i++) {
			/*
			 * When (dirty == true), *all* pages within the run
			 * need to have their dirty bits set, because only
			 * small runs can create a mixture of clean/dirty
			 * pages, but such runs are passed to this function
			 * with (dirty == false).
			 */
			assert((chunk->map[run_ind + i].bits & CHUNK_MAP_DIRTY)
			    == 0);
			chunk->ndirty++;
			arena->ndirty++;
			chunk->map[run_ind + i].bits = CHUNK_MAP_DIRTY;
		}
	} else {
		size_t i;

		for (i = 0; i < run_pages; i++) {
			chunk->map[run_ind + i].bits &= ~(CHUNK_MAP_LARGE |
			    CHUNK_MAP_ALLOCATED);
		}
	}
	chunk->map[run_ind].bits = size | (chunk->map[run_ind].bits &
	    CHUNK_MAP_FLAGS_MASK);
	chunk->map[run_ind+run_pages-1].bits = size |
	    (chunk->map[run_ind+run_pages-1].bits & CHUNK_MAP_FLAGS_MASK);

	/* Try to coalesce forward. */
	if (run_ind + run_pages < chunk_npages &&
	    (chunk->map[run_ind+run_pages].bits & CHUNK_MAP_ALLOCATED) == 0) {
		size_t nrun_size = chunk->map[run_ind+run_pages].bits &
		    ~PAGE_MASK;

		/*
		 * Remove successor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		arena_avail_tree_remove(&arena->runs_avail,
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
	    CHUNK_MAP_ALLOCATED) == 0) {
		size_t prun_size = chunk->map[run_ind-1].bits & ~PAGE_MASK;

		run_ind -= prun_size >> PAGE_SHIFT;

		/*
		 * Remove predecessor from runs_avail; the coalesced run is
		 * inserted later.
		 */
		arena_avail_tree_remove(&arena->runs_avail,
		    &chunk->map[run_ind]);

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
	arena_avail_tree_insert(&arena->runs_avail, &chunk->map[run_ind]);

	/* Deallocate chunk if it is now completely unused. */
	if ((chunk->map[arena_chunk_header_npages].bits & (~PAGE_MASK |
	    CHUNK_MAP_ALLOCATED)) == arena_maxclass)
		arena_chunk_dealloc(arena, chunk);

	if (dirty) {
		if (chunk->dirtied == false) {
			arena_chunk_tree_dirty_insert(&arena->chunks_dirty,
			    chunk);
			chunk->dirtied = true;
		}

		/* Enforce opt_lg_dirty_mult. */
		if (opt_lg_dirty_mult >= 0 && (arena->nactive >>
		    opt_lg_dirty_mult) < arena->ndirty)
			arena_purge(arena);
	}
}

static void
arena_run_trim_head(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT;
	size_t head_npages = (oldsize - newsize) >> PAGE_SHIFT;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * leading run as separately allocated.
	 */
	assert((chunk->map[pageind].bits & CHUNK_MAP_DIRTY) == 0);
	chunk->map[pageind].bits = (oldsize - newsize) | CHUNK_MAP_LARGE |
	    CHUNK_MAP_ALLOCATED;
	assert((chunk->map[pageind+head_npages].bits & CHUNK_MAP_DIRTY) == 0);
	chunk->map[pageind+head_npages].bits = newsize | CHUNK_MAP_LARGE |
	    CHUNK_MAP_ALLOCATED;

	arena_run_dalloc(arena, run, false);
}

static void
arena_run_trim_tail(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    size_t oldsize, size_t newsize, bool dirty)
{
	size_t pageind = ((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT;
	size_t npages = newsize >> PAGE_SHIFT;

	assert(oldsize > newsize);

	/*
	 * Update the chunk map so that arena_run_dalloc() can treat the
	 * trailing run as separately allocated.
	 */
	assert((chunk->map[pageind].bits & CHUNK_MAP_DIRTY) == 0);
	chunk->map[pageind].bits = newsize | CHUNK_MAP_LARGE |
	    CHUNK_MAP_ALLOCATED;
	assert((chunk->map[pageind+npages].bits & CHUNK_MAP_DIRTY) == 0);
	chunk->map[pageind+npages].bits = (oldsize - newsize) | CHUNK_MAP_LARGE
	    | CHUNK_MAP_ALLOCATED;

	arena_run_dalloc(arena, (arena_run_t *)((uintptr_t)run + newsize),
	    dirty);
}

static arena_run_t *
arena_bin_nonfull_run_get(arena_t *arena, arena_bin_t *bin)
{
	arena_chunk_map_t *mapelm;
	arena_run_t *run;
	unsigned i, remainder;

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
		    ((mapelm->bits & CHUNK_MAP_PG_MASK) >> CHUNK_MAP_PG_SHIFT))
		    << PAGE_SHIFT));
#ifdef JEMALLOC_STATS
		bin->stats.reruns++;
#endif
		return (run);
	}
	/* No existing runs have any space available. */

	/* Allocate a new run. */
	run = arena_run_alloc(arena, bin->run_size, false, false);
	if (run == NULL)
		return (NULL);

	/* Initialize run internals. */
	run->bin = bin;

	for (i = 0; i < bin->regs_mask_nelms - 1; i++)
		run->regs_mask[i] = UINT_MAX;
	remainder = bin->nregs & ((1U << (LG_SIZEOF_INT + 3)) - 1);
	if (remainder == 0)
		run->regs_mask[i] = UINT_MAX;
	else {
		/* The last element has spare bits that need to be unset. */
		run->regs_mask[i] = (UINT_MAX >> ((1U << (LG_SIZEOF_INT + 3))
		    - remainder));
	}

	run->regs_minelm = 0;

	run->nfree = bin->nregs;
#ifdef JEMALLOC_DEBUG
	run->magic = ARENA_RUN_MAGIC;
#endif

#ifdef JEMALLOC_STATS
	bin->stats.nruns++;
	bin->stats.curruns++;
	if (bin->stats.curruns > bin->stats.highruns)
		bin->stats.highruns = bin->stats.curruns;
#endif
	return (run);
}

/* bin->runcur must have space available before this function is called. */
static inline void *
arena_bin_malloc_easy(arena_t *arena, arena_bin_t *bin, arena_run_t *run)
{
	void *ret;

	assert(run->magic == ARENA_RUN_MAGIC);
	assert(run->nfree > 0);

	ret = arena_run_reg_alloc(run, bin);
	assert(ret != NULL);
	run->nfree--;

	return (ret);
}

/* Re-fill bin->runcur, then call arena_bin_malloc_easy(). */
static void *
arena_bin_malloc_hard(arena_t *arena, arena_bin_t *bin)
{

	bin->runcur = arena_bin_nonfull_run_get(arena, bin);
	if (bin->runcur == NULL)
		return (NULL);
	assert(bin->runcur->magic == ARENA_RUN_MAGIC);
	assert(bin->runcur->nfree > 0);

	return (arena_bin_malloc_easy(arena, bin, bin->runcur));
}

#ifdef JEMALLOC_TCACHE
void
arena_tcache_fill(arena_t *arena, tcache_bin_t *tbin, size_t binind)
{
	unsigned i, nfill;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;

	assert(tbin->ncached == 0);

	bin = &arena->bins[binind];
	malloc_mutex_lock(&arena->lock);
	for (i = 0, nfill = (tcache_nslots >> 1); i < nfill; i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_bin_malloc_easy(arena, bin, run);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL) {
			if (i > 0) {
				/*
				 * Move valid pointers to the base of
				 * tbin->slots.
				 */
				memmove(&tbin->slots[0],
				    &tbin->slots[nfill - i],
				    i * sizeof(void *));
			}
			break;
		}
		/*
		 * Fill slots such that the objects lowest in memory come last.
		 * This causes tcache to use low objects first.
		 */
		tbin->slots[nfill - 1 - i] = ptr;
	}
#ifdef JEMALLOC_STATS
	bin->stats.nfills++;
	bin->stats.nrequests += tbin->tstats.nrequests;
	if (bin->reg_size <= small_maxclass) {
		arena->stats.nmalloc_small += (i - tbin->ncached);
		arena->stats.allocated_small += (i - tbin->ncached) *
		    bin->reg_size;
		arena->stats.nmalloc_small += tbin->tstats.nrequests;
	} else {
		arena->stats.nmalloc_medium += (i - tbin->ncached);
		arena->stats.allocated_medium += (i - tbin->ncached) *
		    bin->reg_size;
		arena->stats.nmalloc_medium += tbin->tstats.nrequests;
	}
	tbin->tstats.nrequests = 0;
#endif
	malloc_mutex_unlock(&arena->lock);
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
 *   *) bin->run_size <= RUN_MAX_SMALL
 *   *) run header overhead <= RUN_MAX_OVRHD (or header overhead relaxed).
 *   *) run header size < PAGE_SIZE
 *
 * bin->nregs, bin->regs_mask_nelms, and bin->reg0_offset are
 * also calculated here, since these settings are all interdependent.
 */
static size_t
arena_bin_run_size_calc(arena_bin_t *bin, size_t min_run_size)
{
	size_t try_run_size, good_run_size;
	unsigned good_nregs, good_mask_nelms, good_reg0_offset;
	unsigned try_nregs, try_mask_nelms, try_reg0_offset;

	assert(min_run_size >= PAGE_SIZE);
	assert(min_run_size <= arena_maxclass);
	assert(min_run_size <= RUN_MAX_SMALL);

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
		try_mask_nelms = (try_nregs >> (LG_SIZEOF_INT + 3)) +
		    ((try_nregs & ((1U << (LG_SIZEOF_INT + 3)) - 1)) ? 1 : 0);
		try_reg0_offset = try_run_size - (try_nregs * bin->reg_size);
	} while (sizeof(arena_run_t) + (sizeof(unsigned) * (try_mask_nelms - 1))
	    > try_reg0_offset);

	/* run_size expansion loop. */
	do {
		/*
		 * Copy valid settings before trying more aggressive settings.
		 */
		good_run_size = try_run_size;
		good_nregs = try_nregs;
		good_mask_nelms = try_mask_nelms;
		good_reg0_offset = try_reg0_offset;

		/* Try more aggressive settings. */
		try_run_size += PAGE_SIZE;
		try_nregs = ((try_run_size - sizeof(arena_run_t)) /
		    bin->reg_size) + 1; /* Counter-act try_nregs-- in loop. */
		do {
			try_nregs--;
			try_mask_nelms = (try_nregs >> (LG_SIZEOF_INT + 3)) +
			    ((try_nregs & ((1U << (LG_SIZEOF_INT + 3)) - 1)) ?
			    1 : 0);
			try_reg0_offset = try_run_size - (try_nregs *
			    bin->reg_size);
		} while (sizeof(arena_run_t) + (sizeof(unsigned) *
		    (try_mask_nelms - 1)) > try_reg0_offset);
	} while (try_run_size <= arena_maxclass && try_run_size <= RUN_MAX_SMALL
	    && RUN_MAX_OVRHD * (bin->reg_size << 3) > RUN_MAX_OVRHD_RELAX
	    && (try_reg0_offset << RUN_BFP) > RUN_MAX_OVRHD * try_run_size
	    && (sizeof(arena_run_t) + (sizeof(unsigned) * (try_mask_nelms - 1)))
	    < PAGE_SIZE);

	assert(sizeof(arena_run_t) + (sizeof(unsigned) * (good_mask_nelms - 1))
	    <= good_reg0_offset);
	assert((good_mask_nelms << (LG_SIZEOF_INT + 3)) >= good_nregs);

	/* Copy final settings. */
	bin->run_size = good_run_size;
	bin->nregs = good_nregs;
	bin->regs_mask_nelms = good_mask_nelms;
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
	assert(binind < mbin0);
	bin = &arena->bins[binind];
	size = bin->reg_size;

	malloc_mutex_lock(&arena->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_bin_malloc_easy(arena, bin, run);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}

#ifdef JEMALLOC_STATS
#  ifdef JEMALLOC_TCACHE
	if (isthreaded == false) {
#  endif
		bin->stats.nrequests++;
		arena->stats.nmalloc_small++;
#  ifdef JEMALLOC_TCACHE
	}
#  endif
	arena->stats.allocated_small += size;
#endif
	malloc_mutex_unlock(&arena->lock);

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
arena_malloc_medium(arena_t *arena, size_t size, bool zero)
{
	void *ret;
	arena_bin_t *bin;
	arena_run_t *run;
	size_t binind;

	size = MEDIUM_CEILING(size);
	binind = mbin0 + ((size - medium_min) >> lg_mspace);
	assert(binind < nbins);
	bin = &arena->bins[binind];
	assert(bin->reg_size == size);

	malloc_mutex_lock(&arena->lock);
	if ((run = bin->runcur) != NULL && run->nfree > 0)
		ret = arena_bin_malloc_easy(arena, bin, run);
	else
		ret = arena_bin_malloc_hard(arena, bin);

	if (ret == NULL) {
		malloc_mutex_unlock(&arena->lock);
		return (NULL);
	}

#ifdef JEMALLOC_STATS
#  ifdef JEMALLOC_TCACHE
	if (isthreaded == false) {
#  endif
		bin->stats.nrequests++;
		arena->stats.nmalloc_medium++;
#  ifdef JEMALLOC_TCACHE
	}
#  endif
	arena->stats.allocated_medium += size;
#endif
	malloc_mutex_unlock(&arena->lock);

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

static void *
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
	arena->stats.allocated_large += size;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].nrequests++;
	arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns++;
	if (arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns >
	    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns) {
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].highruns =
		    arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns;
	}
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

	if (size <= bin_maxclass) {
#ifdef JEMALLOC_TCACHE
		tcache_t *tcache;

		if ((tcache = tcache_get()) != NULL)
			return (tcache_alloc(tcache, size, zero));
#endif
		if (size <= small_maxclass)
			return (arena_malloc_small(choose_arena(), size, zero));
		else {
			return (arena_malloc_medium(choose_arena(), size,
			    zero));
		}
	} else
		return (arena_malloc_large(choose_arena(), size, zero));
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
	arena->stats.allocated_large += size;
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

static bool
arena_is_large(const void *ptr)
{
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapbits = chunk->map[pageind].bits;
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	return ((mapbits & CHUNK_MAP_LARGE) != 0);
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
		    (uintptr_t)((pageind - ((mapbits & CHUNK_MAP_PG_MASK) >>
		    CHUNK_MAP_PG_SHIFT)) << PAGE_SHIFT));
		assert(run->magic == ARENA_RUN_MAGIC);
		ret = run->bin->reg_size;
	} else {
		ret = mapbits & ~PAGE_MASK;
		assert(ret != 0);
	}

	return (ret);
}

static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin)
{
	size_t run_ind;

	/* Deallocate run. */
	if (run == bin->runcur)
		bin->runcur = NULL;
	else if (bin->nregs != 1) {
		size_t run_pageind = (((uintptr_t)run -
		    (uintptr_t)chunk)) >> PAGE_SHIFT;
		arena_chunk_map_t *run_mapelm =
		    &chunk->map[run_pageind];
		/*
		 * This block's conditional is necessary because if the
		 * run only contains one region, then it never gets
		 * inserted into the non-full runs tree.
		 */
		arena_run_tree_remove(&bin->runs, run_mapelm);
	}
	/*
	 * Mark the first page as dirty.  The dirty bit for every other page in
	 * the run is already properly set, which means we can call
	 * arena_run_dalloc(..., false), thus potentially avoiding the needless
	 * creation of many dirty pages.
	 */
	run_ind = (size_t)(((uintptr_t)run - (uintptr_t)chunk) >> PAGE_SHIFT);
	assert((chunk->map[run_ind].bits & CHUNK_MAP_DIRTY) == 0);
	chunk->map[run_ind].bits |= CHUNK_MAP_DIRTY;
	chunk->ndirty++;
	arena->ndirty++;

#ifdef JEMALLOC_DEBUG
	run->magic = 0;
#endif
	arena_run_dalloc(arena, run, false);
#ifdef JEMALLOC_STATS
	bin->stats.curruns--;
#endif

	if (chunk->dirtied == false) {
		arena_chunk_tree_dirty_insert(&arena->chunks_dirty, chunk);
		chunk->dirtied = true;
	}
	/* Enforce opt_lg_dirty_mult. */
	if (opt_lg_dirty_mult >= 0 && (arena->nactive >> opt_lg_dirty_mult) <
	    arena->ndirty)
		arena_purge(arena);
}

void
arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_t *mapelm)
{
	size_t pageind;
	arena_run_t *run;
	arena_bin_t *bin;
	size_t size;

	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    ((mapelm->bits & CHUNK_MAP_PG_MASK) >> CHUNK_MAP_PG_SHIFT)) <<
	    PAGE_SHIFT));
	assert(run->magic == ARENA_RUN_MAGIC);
	bin = run->bin;
	size = bin->reg_size;

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, size);
#endif

	arena_run_reg_dalloc(run, bin, ptr, size);
	run->nfree++;

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
	if (size <= small_maxclass) {
		arena->stats.allocated_small -= size;
		arena->stats.ndalloc_small++;
	} else {
		arena->stats.allocated_medium -= size;
		arena->stats.ndalloc_medium++;
	}
#endif
}

#ifdef JEMALLOC_STATS
void
arena_stats_merge(arena_t *arena, size_t *nactive, size_t *ndirty,
    arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats)
{
	unsigned i, nlclasses;

	*nactive += arena->nactive;
	*ndirty += arena->ndirty;

	astats->mapped += arena->stats.mapped;
	astats->npurge += arena->stats.npurge;
	astats->nmadvise += arena->stats.nmadvise;
	astats->purged += arena->stats.purged;
	astats->allocated_small += arena->stats.allocated_small;
	astats->nmalloc_small += arena->stats.nmalloc_small;
	astats->ndalloc_small += arena->stats.ndalloc_small;
	astats->allocated_medium += arena->stats.allocated_medium;
	astats->nmalloc_medium += arena->stats.nmalloc_medium;
	astats->ndalloc_medium += arena->stats.ndalloc_medium;
	astats->allocated_large += arena->stats.allocated_large;
	astats->nmalloc_large += arena->stats.nmalloc_large;
	astats->ndalloc_large += arena->stats.ndalloc_large;

	for (i = 0; i < nbins; i++) {
		bstats[i].nrequests += arena->bins[i].stats.nrequests;
#ifdef JEMALLOC_TCACHE
		bstats[i].nfills += arena->bins[i].stats.nfills;
		bstats[i].nflushes += arena->bins[i].stats.nflushes;
#endif
		bstats[i].nruns += arena->bins[i].stats.nruns;
		bstats[i].reruns += arena->bins[i].stats.reruns;
		bstats[i].highruns += arena->bins[i].stats.highruns;
		bstats[i].curruns += arena->bins[i].stats.curruns;
	}

	for (i = 0, nlclasses = (chunksize - PAGE_SIZE) >> PAGE_SHIFT;
	    i < nlclasses;
	    i++) {
		lstats[i].nrequests += arena->stats.lstats[i].nrequests;
		lstats[i].highruns += arena->stats.lstats[i].highruns;
		lstats[i].curruns += arena->stats.lstats[i].curruns;
	}
}

static void
arena_stats_aprint(size_t nactive, size_t ndirty, const arena_stats_t *astats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque)
{

	malloc_cprintf(write4, w4opaque,
	    "dirty pages: %zu:%zu active:dirty, %llu sweep%s,"
	    " %llu madvise%s, %llu purged\n",
	    nactive, ndirty,
	    astats->npurge, astats->npurge == 1 ? "" : "s",
	    astats->nmadvise, astats->nmadvise == 1 ? "" : "s",
	    astats->purged);

	malloc_cprintf(write4, w4opaque,
	    "            allocated      nmalloc      ndalloc\n");
	malloc_cprintf(write4, w4opaque, "small:   %12zu %12llu %12llu\n",
	    astats->allocated_small, astats->nmalloc_small,
	    astats->ndalloc_small);
	malloc_cprintf(write4, w4opaque, "medium:  %12zu %12llu %12llu\n",
	    astats->allocated_medium, astats->nmalloc_medium,
	    astats->ndalloc_medium);
	malloc_cprintf(write4, w4opaque, "large:   %12zu %12llu %12llu\n",
	    astats->allocated_large, astats->nmalloc_large,
	    astats->ndalloc_large);
	malloc_cprintf(write4, w4opaque, "total:   %12zu %12llu %12llu\n",
	    astats->allocated_small + astats->allocated_medium +
	    astats->allocated_large, astats->nmalloc_small +
	    astats->nmalloc_medium + astats->nmalloc_large,
	    astats->ndalloc_small + astats->ndalloc_medium +
	    astats->ndalloc_large);
	malloc_cprintf(write4, w4opaque, "mapped:  %12zu\n", astats->mapped);
}

static void
arena_stats_bprint(arena_t *arena, const malloc_bin_stats_t *bstats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque)
{
	unsigned i, gap_start;

#ifdef JEMALLOC_TCACHE
	malloc_cprintf(write4, w4opaque,
	    "bins:     bin    size regs pgs  requests    "
	    "nfills  nflushes   newruns    reruns maxruns curruns\n");
#else
	malloc_cprintf(write4, w4opaque,
	    "bins:     bin    size regs pgs  requests   "
	    "newruns    reruns maxruns curruns\n");
#endif
	for (i = 0, gap_start = UINT_MAX; i < nbins; i++) {
		if (bstats[i].nruns == 0) {
			if (gap_start == UINT_MAX)
				gap_start = i;
		} else {
			if (gap_start != UINT_MAX) {
				if (i > gap_start + 1) {
					/* Gap of more than one size class. */
					malloc_cprintf(write4, w4opaque,
					    "[%u..%u]\n", gap_start,
					    i - 1);
				} else {
					/* Gap of one size class. */
					malloc_cprintf(write4, w4opaque,
					    "[%u]\n", gap_start);
				}
				gap_start = UINT_MAX;
			}
			malloc_cprintf(write4, w4opaque,
			    "%13u %1s %5u %4u %3u %9llu %9llu"
#ifdef JEMALLOC_TCACHE
			    " %9llu %9llu"
#endif
			    " %9llu %7zu %7zu\n",
			    i,
			    i < ntbins ? "T" : i < ntbins + nqbins ?
			    "Q" : i < ntbins + nqbins + ncbins ? "C" :
			    i < ntbins + nqbins + ncbins + nsbins ? "S"
			    : "M",
			    arena->bins[i].reg_size,
			    arena->bins[i].nregs,
			    arena->bins[i].run_size >> PAGE_SHIFT,
			    bstats[i].nrequests,
#ifdef JEMALLOC_TCACHE
			    bstats[i].nfills,
			    bstats[i].nflushes,
#endif
			    bstats[i].nruns,
			    bstats[i].reruns,
			    bstats[i].highruns,
			    bstats[i].curruns);
		}
	}
	if (gap_start != UINT_MAX) {
		if (i > gap_start + 1) {
			/* Gap of more than one size class. */
			malloc_cprintf(write4, w4opaque, "[%u..%u]\n",
			    gap_start, i - 1);
		} else {
			/* Gap of one size class. */
			malloc_cprintf(write4, w4opaque, "[%u]\n", gap_start);
		}
	}
}

static void
arena_stats_lprint(const malloc_large_stats_t *lstats,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque)
{
	size_t i;
	ssize_t gap_start;
	size_t nlclasses = (chunksize - PAGE_SIZE) >> PAGE_SHIFT;

	malloc_cprintf(write4, w4opaque,
	    "large:   size pages nrequests   maxruns   curruns\n");

	for (i = 0, gap_start = -1; i < nlclasses; i++) {
		if (lstats[i].nrequests == 0) {
			if (gap_start == -1)
				gap_start = i;
		} else {
			if (gap_start != -1) {
				malloc_cprintf(write4, w4opaque, "[%zu]\n",
				    i - gap_start);
				gap_start = -1;
			}
			malloc_cprintf(write4, w4opaque,
			    "%13zu %5zu %9llu %9zu %9zu\n",
			    (i+1) << PAGE_SHIFT, i+1,
			    lstats[i].nrequests,
			    lstats[i].highruns,
			    lstats[i].curruns);
		}
	}
	if (gap_start != -1)
		malloc_cprintf(write4, w4opaque, "[%zu]\n", i - gap_start);
}

void
arena_stats_mprint(arena_t *arena, size_t nactive, size_t ndirty,
    const arena_stats_t *astats, const malloc_bin_stats_t *bstats,
    const malloc_large_stats_t *lstats, bool bins, bool large,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque)
{

	arena_stats_aprint(nactive, ndirty, astats, write4, w4opaque);
	if (bins && astats->nmalloc_small + astats->nmalloc_medium > 0)
		arena_stats_bprint(arena, bstats, write4, w4opaque);
	if (large && astats->nmalloc_large > 0)
		arena_stats_lprint(lstats, write4, w4opaque);
}

void
arena_stats_print(arena_t *arena, bool bins, bool large,
    void (*write4)(void *, const char *, const char *, const char *,
    const char *), void *w4opaque)
{
	size_t nactive, ndirty;
	arena_stats_t astats;
	malloc_bin_stats_t bstats[nbins];
	malloc_large_stats_t lstats[((chunksize - PAGE_SIZE) >> PAGE_SHIFT)];

	nactive = 0;
	ndirty = 0;
	memset(&astats, 0, sizeof(astats));
	memset(bstats, 0, sizeof(bstats));
	memset(lstats, 0, sizeof(lstats));

	arena_stats_merge(arena, &nactive, &ndirty, &astats, bstats, lstats);
	arena_stats_mprint(arena, nactive, ndirty, &astats, bstats, lstats,
	    bins, large, write4, w4opaque);
}
#endif

void
arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{
	/* Large allocation. */
	malloc_mutex_lock(&arena->lock);

#ifdef JEMALLOC_FILL
#ifndef JEMALLOC_STATS
	if (opt_junk)
#endif
#endif
	{
#if (defined(JEMALLOC_FILL) || defined(JEMALLOC_STATS))
		size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >>
		    PAGE_SHIFT;
		size_t size = chunk->map[pageind].bits & ~PAGE_MASK;
#endif

#ifdef JEMALLOC_FILL
#ifdef JEMALLOC_STATS
		if (opt_junk)
#endif
			memset(ptr, 0x5a, size);
#endif
#ifdef JEMALLOC_STATS
		arena->stats.allocated_large -= size;
		arena->stats.lstats[(size >> PAGE_SHIFT) - 1].curruns--;
#endif
	}
#ifdef JEMALLOC_STATS
	arena->stats.ndalloc_large++;
#endif

	arena_run_dalloc(arena, (arena_run_t *)ptr, true);
	malloc_mutex_unlock(&arena->lock);
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
	arena->stats.allocated_large -= oldsize - size;
	arena->stats.lstats[size >> PAGE_SHIFT].nrequests++;
	arena->stats.lstats[size >> PAGE_SHIFT].curruns++;
	if (arena->stats.lstats[size >> PAGE_SHIFT].curruns >
	    arena->stats.lstats[size >> PAGE_SHIFT].highruns) {
		arena->stats.lstats[size >> PAGE_SHIFT].highruns =
		    arena->stats.lstats[size >> PAGE_SHIFT].curruns;
	}
	arena->stats.lstats[oldsize >> PAGE_SHIFT].curruns--;
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
		arena->stats.allocated_large += size - oldsize;
		arena->stats.lstats[size >> PAGE_SHIFT].nrequests++;
		arena->stats.lstats[size >> PAGE_SHIFT].curruns++;
		if (arena->stats.lstats[size >> PAGE_SHIFT].curruns >
		    arena->stats.lstats[size >> PAGE_SHIFT].highruns) {
			arena->stats.lstats[size >> PAGE_SHIFT].highruns =
			    arena->stats.lstats[size >> PAGE_SHIFT].curruns;
		}
		arena->stats.lstats[oldsize >> PAGE_SHIFT].curruns--;
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

	/*
	 * Try to avoid moving the allocation.
	 *
	 * posix_memalign() can cause allocation of "large" objects that are
	 * smaller than bin_maxclass (in order to meet alignment requirements).
	 * Therefore, do not assume that (oldsize <= bin_maxclass) indicates
	 * ptr refers to a bin-allocated object.
	 */
	if (oldsize <= arena_maxclass) {
		if (arena_is_large(ptr) == false ) {
			if (size <= small_maxclass) {
				if (oldsize <= small_maxclass &&
				    small_size2bin[size] ==
				    small_size2bin[oldsize])
					goto IN_PLACE;
			} else if (size <= bin_maxclass) {
				if (small_maxclass < oldsize && oldsize <=
				    bin_maxclass && MEDIUM_CEILING(size) ==
				    MEDIUM_CEILING(oldsize))
					goto IN_PLACE;
			}
		} else {
			assert(size <= arena_maxclass);
			if (size > bin_maxclass) {
				if (arena_ralloc_large(ptr, size, oldsize) ==
				    false)
					return (ptr);
			}
		}
	}

	/* Try to avoid moving the allocation. */
	if (size <= small_maxclass) {
		if (oldsize <= small_maxclass && small_size2bin[size] ==
		    small_size2bin[oldsize])
			goto IN_PLACE;
	} else if (size <= bin_maxclass) {
		if (small_maxclass < oldsize && oldsize <= bin_maxclass &&
		    MEDIUM_CEILING(size) == MEDIUM_CEILING(oldsize))
			goto IN_PLACE;
	} else {
		if (bin_maxclass < oldsize && oldsize <= arena_maxclass) {
			assert(size > bin_maxclass);
			if (arena_ralloc_large(ptr, size, oldsize) == false)
				return (ptr);
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

	if (malloc_mutex_init(&arena->lock))
		return (true);

#ifdef JEMALLOC_STATS
	memset(&arena->stats, 0, sizeof(arena_stats_t));
	arena->stats.lstats = (malloc_large_stats_t *)base_alloc(
	    sizeof(malloc_large_stats_t) * ((chunksize - PAGE_SIZE) >>
	        PAGE_SHIFT));
	if (arena->stats.lstats == NULL)
		return (true);
	memset(arena->stats.lstats, 0, sizeof(malloc_large_stats_t) *
	    ((chunksize - PAGE_SIZE) >> PAGE_SHIFT));
#  ifdef JEMALLOC_TCACHE
	ql_new(&arena->tcache_ql);
#  endif
#endif

#ifdef JEMALLOC_TRACE
	if (opt_trace) {
		/* "jemtr.<pid>.<arena>" */
		char buf[UMAX2S_BUFSIZE];
		char filename[6 + UMAX2S_BUFSIZE + 1 + UMAX2S_BUFSIZE + 1];
		char *s;
		unsigned i, slen;

		arena->trace_buf_end = 0;

		i = 0;

		s = "jemtr.";
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;

		s = umax2s(getpid(), 10, buf);
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;

		s = ".";
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;

		s = umax2s(ind, 10, buf);
		slen = strlen(s);
		memcpy(&filename[i], s, slen);
		i += slen;

		filename[i] = '\0';

		arena->trace_fd = creat(filename, 0644);
		if (arena->trace_fd == -1) {
			malloc_write4("<jemalloc>",
			    ": creat(\"", filename, "\", O_RDWR) failed\n");
			abort();
		}
	}
#endif

	/* Initialize chunks. */
	arena_chunk_tree_dirty_new(&arena->chunks_dirty);
	arena->spare = NULL;

	arena->nactive = 0;
	arena->ndirty = 0;

	arena_avail_tree_new(&arena->runs_avail);

	/* Initialize bins. */
	prev_run_size = PAGE_SIZE;

	i = 0;
#ifdef JEMALLOC_TINY
	/* (2^n)-spaced tiny bins. */
	for (; i < ntbins; i++) {
		bin = &arena->bins[i];
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
	for (; i < ntbins + nqbins + ncbins + nsbins; i++) {
		bin = &arena->bins[i];
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = sspace_min + ((i - (ntbins + nqbins + ncbins))
		    << LG_SUBPAGE);

		prev_run_size = arena_bin_run_size_calc(bin, prev_run_size);

#ifdef JEMALLOC_STATS
		memset(&bin->stats, 0, sizeof(malloc_bin_stats_t));
#endif
	}

	/* Medium bins. */
	for (; i < nbins; i++) {
		bin = &arena->bins[i];
		bin->runcur = NULL;
		arena_run_tree_new(&bin->runs);

		bin->reg_size = medium_min + ((i - (ntbins + nqbins + ncbins +
		    nsbins)) << lg_mspace);

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
arena_boot0(void)
{

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
	medium_max = (1U << opt_lg_medium_max);

#ifdef JEMALLOC_TINY
	assert(LG_QUANTUM >= LG_TINY_MIN);
#endif
	assert(ntbins <= LG_QUANTUM);
	nqbins = qspace_max >> LG_QUANTUM;
	ncbins = ((cspace_max - cspace_min) >> LG_CACHELINE) + 1;
	nsbins = ((sspace_max - sspace_min) >> LG_SUBPAGE) + 1;

	/*
	 * Compute medium size class spacing and the number of medium size
	 * classes.  Limit spacing to no more than pagesize, but if possible
	 * use the smallest spacing that does not exceed NMBINS_MAX medium size
	 * classes.
	 */
	lg_mspace = LG_SUBPAGE;
	nmbins = ((medium_max - medium_min) >> lg_mspace) + 1;
	while (lg_mspace < PAGE_SHIFT && nmbins > NMBINS_MAX) {
		lg_mspace = lg_mspace + 1;
		nmbins = ((medium_max - medium_min) >> lg_mspace) + 1;
	}
	mspace_mask = (1U << lg_mspace) - 1U;

	mbin0 = ntbins + nqbins + ncbins + nsbins;
	nbins = mbin0 + nmbins;
	/*
	 * The small_size2bin lookup table uses uint8_t to encode each bin
	 * index, so we cannot support more than 256 small size classes.  This
	 * limit is difficult to exceed (not even possible with 16B quantum and
	 * 4KiB pages), and such configurations are impractical, but
	 * nonetheless we need to protect against this case in order to avoid
	 * undefined behavior.
	 */
	if (mbin0 > 256) {
	    char line_buf[UMAX2S_BUFSIZE];
	    malloc_write4("<jemalloc>: Too many small size classes (",
	        umax2s(mbin0, 10, line_buf), " > max 256)\n", "");
	    abort();
	}

	if (small_size2bin_init())
		return (true);

	return (false);
}

void
arena_boot1(void)
{
	size_t header_size;

	/*
	 * Compute the header size such that it is large enough to contain the
	 * page map.
	 */
	header_size = sizeof(arena_chunk_t) +
	    (sizeof(arena_chunk_map_t) * (chunk_npages - 1));
	arena_chunk_header_npages = (header_size >> PAGE_SHIFT) +
	    ((header_size & PAGE_MASK) != 0);
	arena_maxclass = chunksize - (arena_chunk_header_npages << PAGE_SHIFT);
}
