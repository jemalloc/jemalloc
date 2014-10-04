/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/* Maximum number of regions in one run. */
#define	LG_RUN_MAXREGS		(LG_PAGE - LG_TINY_MIN)
#define	RUN_MAXREGS		(1U << LG_RUN_MAXREGS)

/*
 * Minimum redzone size.  Redzones may be larger than this if necessary to
 * preserve region alignment.
 */
#define	REDZONE_MINSIZE		16

/*
 * The minimum ratio of active:dirty pages per arena is computed as:
 *
 *   (nactive >> opt_lg_dirty_mult) >= ndirty
 *
 * So, supposing that opt_lg_dirty_mult is 3, there can be no less than 8 times
 * as many active pages as dirty pages.
 */
#define	LG_DIRTY_MULT_DEFAULT	3

typedef struct arena_run_s arena_run_t;
typedef struct arena_chunk_map_bits_s arena_chunk_map_bits_t;
typedef struct arena_chunk_map_misc_s arena_chunk_map_misc_t;
typedef struct arena_chunk_s arena_chunk_t;
typedef struct arena_bin_info_s arena_bin_info_t;
typedef struct arena_bin_s arena_bin_t;
typedef struct arena_s arena_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct arena_run_s {
	/* Bin this run is associated with. */
	arena_bin_t	*bin;

	/* Index of next region that has never been allocated, or nregs. */
	uint32_t	nextind;

	/* Number of free regions in run. */
	unsigned	nfree;

	/* Per region allocated/deallocated bitmap. */
	bitmap_t	bitmap[BITMAP_GROUPS_MAX];
};

/* Each element of the chunk map corresponds to one page within the chunk. */
struct arena_chunk_map_bits_s {
	/*
	 * Run address (or size) and various flags are stored together.  The bit
	 * layout looks like (assuming 32-bit system):
	 *
	 *   ???????? ???????? ????nnnn nnnndula
	 *
	 * ? : Unallocated: Run address for first/last pages, unset for internal
	 *                  pages.
	 *     Small: Run page offset.
	 *     Large: Run size for first page, unset for trailing pages.
	 * n : binind for small size class, BININD_INVALID for large size class.
	 * d : dirty?
	 * u : unzeroed?
	 * l : large?
	 * a : allocated?
	 *
	 * Following are example bit patterns for the three types of runs.
	 *
	 * p : run page offset
	 * s : run size
	 * n : binind for size class; large objects set these to BININD_INVALID
	 * x : don't care
	 * - : 0
	 * + : 1
	 * [DULA] : bit set
	 * [dula] : bit unset
	 *
	 *   Unallocated (clean):
	 *     ssssssss ssssssss ssss++++ ++++du-a
	 *     xxxxxxxx xxxxxxxx xxxxxxxx xxxx-Uxx
	 *     ssssssss ssssssss ssss++++ ++++dU-a
	 *
	 *   Unallocated (dirty):
	 *     ssssssss ssssssss ssss++++ ++++D--a
	 *     xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
	 *     ssssssss ssssssss ssss++++ ++++D--a
	 *
	 *   Small:
	 *     pppppppp pppppppp ppppnnnn nnnnd--A
	 *     pppppppp pppppppp ppppnnnn nnnn---A
	 *     pppppppp pppppppp ppppnnnn nnnnd--A
	 *
	 *   Large:
	 *     ssssssss ssssssss ssss++++ ++++D-LA
	 *     xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
	 *     -------- -------- ----++++ ++++D-LA
	 *
	 *   Large (sampled, size <= PAGE):
	 *     ssssssss ssssssss ssssnnnn nnnnD-LA
	 *
	 *   Large (not sampled, size == PAGE):
	 *     ssssssss ssssssss ssss++++ ++++D-LA
	 */
	size_t				bits;
#define	CHUNK_MAP_BININD_SHIFT	4
#define	BININD_INVALID		((size_t)0xffU)
/*     CHUNK_MAP_BININD_MASK == (BININD_INVALID << CHUNK_MAP_BININD_SHIFT) */
#define	CHUNK_MAP_BININD_MASK	((size_t)0xff0U)
#define	CHUNK_MAP_BININD_INVALID CHUNK_MAP_BININD_MASK
#define	CHUNK_MAP_FLAGS_MASK	((size_t)0xcU)
#define	CHUNK_MAP_DIRTY		((size_t)0x8U)
#define	CHUNK_MAP_UNZEROED	((size_t)0x4U)
#define	CHUNK_MAP_LARGE		((size_t)0x2U)
#define	CHUNK_MAP_ALLOCATED	((size_t)0x1U)
#define	CHUNK_MAP_KEY		CHUNK_MAP_ALLOCATED
};

/*
 * Each arena_chunk_map_misc_t corresponds to one page within the chunk, just
 * like arena_chunk_map_bits_t.  Two separate arrays are stored within each
 * chunk header in order to improve cache locality.
 */
struct arena_chunk_map_misc_s {
	/*
	 * Linkage for run trees.  There are two disjoint uses:
	 *
	 * 1) arena_t's runs_avail tree.
	 * 2) arena_run_t conceptually uses this linkage for in-use non-full
	 * runs, rather than directly embedding linkage.
	 */
	rb_node(arena_chunk_map_misc_t)		rb_link;

	union {
		/* Linkage for list of dirty runs. */
		ql_elm(arena_chunk_map_misc_t)	dr_link;

		/* Profile counters, used for large object runs. */
		prof_tctx_t			*prof_tctx;

		/* Small region run metadata. */
		arena_run_t			run;
	};
};
typedef rb_tree(arena_chunk_map_misc_t) arena_avail_tree_t;
typedef rb_tree(arena_chunk_map_misc_t) arena_run_tree_t;
typedef ql_head(arena_chunk_map_misc_t) arena_chunk_miscelms_t;

/* Arena chunk header. */
struct arena_chunk_s {
	/* Arena that owns the chunk. */
	arena_t			*arena;

	/*
	 * Map of pages within chunk that keeps track of free/large/small.  The
	 * first map_bias entries are omitted, since the chunk header does not
	 * need to be tracked in the map.  This omission saves a header page
	 * for common chunk sizes (e.g. 4 MiB).
	 */
	arena_chunk_map_bits_t	map_bits[1]; /* Dynamically sized. */
};

/*
 * Read-only information associated with each element of arena_t's bins array
 * is stored separately, partly to reduce memory usage (only one copy, rather
 * than one per arena), but mainly to avoid false cacheline sharing.
 *
 * Each run has the following layout:
 *
 *               /--------------------\
 *               | pad?               |
 *               |--------------------|
 *               | redzone            |
 *   reg0_offset | region 0           |
 *               | redzone            |
 *               |--------------------| \
 *               | redzone            | |
 *               | region 1           |  > reg_interval
 *               | redzone            | /
 *               |--------------------|
 *               | ...                |
 *               | ...                |
 *               | ...                |
 *               |--------------------|
 *               | redzone            |
 *               | region nregs-1     |
 *               | redzone            |
 *               |--------------------|
 *               | alignment pad?     |
 *               \--------------------/
 *
 * reg_interval has at least the same minimum alignment as reg_size; this
 * preserves the alignment constraint that sa2u() depends on.  Alignment pad is
 * either 0 or redzone_size; it is present only if needed to align reg0_offset.
 */
struct arena_bin_info_s {
	/* Size of regions in a run for this bin's size class. */
	size_t		reg_size;

	/* Redzone size. */
	size_t		redzone_size;

	/* Interval between regions (reg_size + (redzone_size << 1)). */
	size_t		reg_interval;

	/* Total size of a run for this bin's size class. */
	size_t		run_size;

	/* Total number of regions in a run for this bin's size class. */
	uint32_t	nregs;

	/*
	 * Metadata used to manipulate bitmaps for runs associated with this
	 * bin.
	 */
	bitmap_info_t	bitmap_info;

	/* Offset of first region in a run for this bin's size class. */
	uint32_t	reg0_offset;
};

struct arena_bin_s {
	/*
	 * All operations on runcur, runs, and stats require that lock be
	 * locked.  Run allocation/deallocation are protected by the arena lock,
	 * which may be acquired while holding one or more bin locks, but not
	 * vise versa.
	 */
	malloc_mutex_t	lock;

	/*
	 * Current run being used to service allocations of this bin's size
	 * class.
	 */
	arena_run_t	*runcur;

	/*
	 * Tree of non-full runs.  This tree is used when looking for an
	 * existing run when runcur is no longer usable.  We choose the
	 * non-full run that is lowest in memory; this policy tends to keep
	 * objects packed well, and it can also help reduce the number of
	 * almost-empty chunks.
	 */
	arena_run_tree_t runs;

	/* Bin statistics. */
	malloc_bin_stats_t stats;
};

struct arena_s {
	/* This arena's index within the arenas array. */
	unsigned		ind;

	/*
	 * Number of threads currently assigned to this arena.  This field is
	 * protected by arenas_lock.
	 */
	unsigned		nthreads;

	/*
	 * There are three classes of arena operations from a locking
	 * perspective:
	 * 1) Thread asssignment (modifies nthreads) is protected by
	 *    arenas_lock.
	 * 2) Bin-related operations are protected by bin locks.
	 * 3) Chunk- and run-related operations are protected by this mutex.
	 */
	malloc_mutex_t		lock;

	arena_stats_t		stats;
	/*
	 * List of tcaches for extant threads associated with this arena.
	 * Stats from these are merged incrementally, and at exit.
	 */
	ql_head(tcache_t)	tcache_ql;

	uint64_t		prof_accumbytes;

	dss_prec_t		dss_prec;

	/*
	 * In order to avoid rapid chunk allocation/deallocation when an arena
	 * oscillates right on the cusp of needing a new chunk, cache the most
	 * recently freed chunk.  The spare is left in the arena's chunk trees
	 * until it is deleted.
	 *
	 * There is one spare chunk per arena, rather than one spare total, in
	 * order to avoid interactions between multiple threads that could make
	 * a single spare inadequate.
	 */
	arena_chunk_t		*spare;

	/* Number of pages in active runs and huge regions. */
	size_t			nactive;

	/*
	 * Current count of pages within unused runs that are potentially
	 * dirty, and for which madvise(... MADV_DONTNEED) has not been called.
	 * By tracking this, we can institute a limit on how much dirty unused
	 * memory is mapped for each arena.
	 */
	size_t			ndirty;

	/*
	 * Size/address-ordered trees of this arena's available runs.  The trees
	 * are used for first-best-fit run allocation.
	 */
	arena_avail_tree_t	runs_avail;

	/* List of dirty runs this arena manages. */
	arena_chunk_miscelms_t	runs_dirty;

	/*
	 * user-configureable chunk allocation and deallocation functions.
	 */
	chunk_alloc_t		*chunk_alloc;
	chunk_dalloc_t		*chunk_dalloc;

	/* bins is used to store trees of free regions. */
	arena_bin_t		bins[NBINS];
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern ssize_t	opt_lg_dirty_mult;
/*
 * small_size2bin_tab is a compact lookup table that rounds request sizes up to
 * size classes.  In order to reduce cache footprint, the table is compressed,
 * and all accesses are via small_size2bin().
 */
extern uint8_t const	small_size2bin_tab[];
/*
 * small_bin2size_tab duplicates information in arena_bin_info, but in a const
 * array, for which it is easier for the compiler to optimize repeated
 * dereferences.
 */
extern uint32_t const	small_bin2size_tab[NBINS];

extern arena_bin_info_t	arena_bin_info[NBINS];

/* Number of large size classes. */
#define			nlclasses (chunk_npages - map_bias)

void	*arena_chunk_alloc_huge(arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero);
void	arena_chunk_dalloc_huge(arena_t *arena, void *chunk, size_t size);
void	arena_purge_all(arena_t *arena);
void	arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin,
    size_t binind, uint64_t prof_accumbytes);
void	arena_alloc_junk_small(void *ptr, arena_bin_info_t *bin_info,
    bool zero);
#ifdef JEMALLOC_JET
typedef void (arena_redzone_corruption_t)(void *, size_t, bool, size_t,
    uint8_t);
extern arena_redzone_corruption_t *arena_redzone_corruption;
typedef void (arena_dalloc_junk_small_t)(void *, arena_bin_info_t *);
extern arena_dalloc_junk_small_t *arena_dalloc_junk_small;
#else
void	arena_dalloc_junk_small(void *ptr, arena_bin_info_t *bin_info);
#endif
void	arena_quarantine_junk_small(void *ptr, size_t usize);
void	*arena_malloc_small(arena_t *arena, size_t size, bool zero);
void	*arena_malloc_large(arena_t *arena, size_t size, bool zero);
void	*arena_palloc(arena_t *arena, size_t size, size_t alignment, bool zero);
void	arena_prof_promoted(const void *ptr, size_t size);
void	arena_dalloc_bin_locked(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_bits_t *bitselm);
void	arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind, arena_chunk_map_bits_t *bitselm);
void	arena_dalloc_small(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    size_t pageind);
#ifdef JEMALLOC_JET
typedef void (arena_dalloc_junk_large_t)(void *, size_t);
extern arena_dalloc_junk_large_t *arena_dalloc_junk_large;
#endif
void	arena_dalloc_large_locked(arena_t *arena, arena_chunk_t *chunk,
    void *ptr);
void	arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr);
#ifdef JEMALLOC_JET
typedef void (arena_ralloc_junk_large_t)(void *, size_t, size_t);
extern arena_ralloc_junk_large_t *arena_ralloc_junk_large;
#endif
bool	arena_ralloc_no_move(void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero);
void	*arena_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t extra, size_t alignment, bool zero,
    bool try_tcache_alloc, bool try_tcache_dalloc);
dss_prec_t	arena_dss_prec_get(arena_t *arena);
bool	arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec);
void	arena_stats_merge(arena_t *arena, const char **dss, size_t *nactive,
    size_t *ndirty, arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats);
bool	arena_new(arena_t *arena, unsigned ind);
void	arena_boot(void);
void	arena_prefork(arena_t *arena);
void	arena_postfork_parent(arena_t *arena);
void	arena_postfork_child(arena_t *arena);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
size_t	small_size2bin_compute(size_t size);
size_t	small_size2bin_lookup(size_t size);
size_t	small_size2bin(size_t size);
size_t	small_bin2size_compute(size_t binind);
size_t	small_bin2size_lookup(size_t binind);
size_t	small_bin2size(size_t binind);
size_t	small_s2u_compute(size_t size);
size_t	small_s2u_lookup(size_t size);
size_t	small_s2u(size_t size);
arena_chunk_map_bits_t	*arena_bitselm_get(arena_chunk_t *chunk,
    size_t pageind);
arena_chunk_map_misc_t	*arena_miscelm_get(arena_chunk_t *chunk,
    size_t pageind);
size_t	arena_miscelm_to_pageind(arena_chunk_map_misc_t *miscelm);
void	*arena_miscelm_to_rpages(arena_chunk_map_misc_t *miscelm);
arena_chunk_map_misc_t	*arena_run_to_miscelm(arena_run_t *run);
size_t	*arena_mapbitsp_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbitsp_read(size_t *mapbitsp);
size_t	arena_mapbits_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_unallocated_size_get(arena_chunk_t *chunk,
    size_t pageind);
size_t	arena_mapbits_large_size_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_small_runind_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_binind_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_dirty_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_unzeroed_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_large_get(arena_chunk_t *chunk, size_t pageind);
size_t	arena_mapbits_allocated_get(arena_chunk_t *chunk, size_t pageind);
void	arena_mapbitsp_write(size_t *mapbitsp, size_t mapbits);
void	arena_mapbits_unallocated_set(arena_chunk_t *chunk, size_t pageind,
    size_t size, size_t flags);
void	arena_mapbits_unallocated_size_set(arena_chunk_t *chunk, size_t pageind,
    size_t size);
void	arena_mapbits_large_set(arena_chunk_t *chunk, size_t pageind,
    size_t size, size_t flags);
void	arena_mapbits_large_binind_set(arena_chunk_t *chunk, size_t pageind,
    size_t binind);
void	arena_mapbits_small_set(arena_chunk_t *chunk, size_t pageind,
    size_t runind, size_t binind, size_t flags);
void	arena_mapbits_unzeroed_set(arena_chunk_t *chunk, size_t pageind,
    size_t unzeroed);
bool	arena_prof_accum_impl(arena_t *arena, uint64_t accumbytes);
bool	arena_prof_accum_locked(arena_t *arena, uint64_t accumbytes);
bool	arena_prof_accum(arena_t *arena, uint64_t accumbytes);
size_t	arena_ptr_small_binind_get(const void *ptr, size_t mapbits);
size_t	arena_bin_index(arena_t *arena, arena_bin_t *bin);
unsigned	arena_run_regind(arena_run_t *run, arena_bin_info_t *bin_info,
    const void *ptr);
prof_tctx_t	*arena_prof_tctx_get(const void *ptr);
void	arena_prof_tctx_set(const void *ptr, prof_tctx_t *tctx);
void	*arena_malloc(tsd_t *tsd, arena_t *arena, size_t size, bool zero,
    bool try_tcache);
size_t	arena_salloc(const void *ptr, bool demote);
void	arena_dalloc(tsd_t *tsd, arena_chunk_t *chunk, void *ptr,
    bool try_tcache);
void	arena_sdalloc(tsd_t *tsd, arena_chunk_t *chunk, void *ptr, size_t size,
    bool try_tcache);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ARENA_C_))
#  ifdef JEMALLOC_ARENA_INLINE_A
JEMALLOC_INLINE size_t
small_size2bin_compute(size_t size)
{
#if (NTBINS != 0)
	if (size <= (ZU(1) << LG_TINY_MAXCLASS)) {
		size_t lg_tmin = LG_TINY_MAXCLASS - NTBINS + 1;
		size_t lg_ceil = lg_floor(pow2_ceil(size));
		return (lg_ceil < lg_tmin ? 0 : lg_ceil - lg_tmin);
	} else
#endif
	{
		size_t x = lg_floor((size<<1)-1);
		size_t shift = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM) ? 0 :
		    x - (LG_SIZE_CLASS_GROUP + LG_QUANTUM);
		size_t grp = shift << LG_SIZE_CLASS_GROUP;

		size_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM + 1)
		    ? LG_QUANTUM : x - LG_SIZE_CLASS_GROUP - 1;

		size_t delta_inverse_mask = ZI(-1) << lg_delta;
		size_t mod = ((((size-1) & delta_inverse_mask) >> lg_delta)) &
		    ((ZU(1) << LG_SIZE_CLASS_GROUP) - 1);

		size_t bin = NTBINS + grp + mod;
		return (bin);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
small_size2bin_lookup(size_t size)
{

	assert(size <= LOOKUP_MAXCLASS);
	{
		size_t ret = ((size_t)(small_size2bin_tab[(size-1) >>
		    LG_TINY_MIN]));
		assert(ret == small_size2bin_compute(size));
		return (ret);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
small_size2bin(size_t size)
{

	assert(size > 0);
	if (likely(size <= LOOKUP_MAXCLASS))
		return (small_size2bin_lookup(size));
	else
		return (small_size2bin_compute(size));
}

JEMALLOC_INLINE size_t
small_bin2size_compute(size_t binind)
{
#if (NTBINS > 0)
	if (binind < NTBINS)
		return (ZU(1) << (LG_TINY_MAXCLASS - NTBINS + 1 + binind));
	else
#endif
	{
		size_t reduced_binind = binind - NTBINS;
		size_t grp = reduced_binind >> LG_SIZE_CLASS_GROUP;
		size_t mod = reduced_binind & ((ZU(1) << LG_SIZE_CLASS_GROUP) -
		    1);

		size_t grp_size_mask = ~((!!grp)-1);
		size_t grp_size = ((ZU(1) << (LG_QUANTUM +
		    (LG_SIZE_CLASS_GROUP-1))) << grp) & grp_size_mask;

		size_t shift = (grp == 0) ? 1 : grp;
		size_t lg_delta = shift + (LG_QUANTUM-1);
		size_t mod_size = (mod+1) << lg_delta;

		size_t usize = grp_size + mod_size;
		return (usize);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
small_bin2size_lookup(size_t binind)
{

	assert(binind < NBINS);
	{
		size_t ret = (size_t)small_bin2size_tab[binind];
		assert(ret == small_bin2size_compute(binind));
		return (ret);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
small_bin2size(size_t binind)
{

	return (small_bin2size_lookup(binind));
}

JEMALLOC_ALWAYS_INLINE size_t
small_s2u_compute(size_t size)
{
#if (NTBINS > 0)
	if (size <= (ZU(1) << LG_TINY_MAXCLASS)) {
		size_t lg_tmin = LG_TINY_MAXCLASS - NTBINS + 1;
		size_t lg_ceil = lg_floor(pow2_ceil(size));
		return (lg_ceil < lg_tmin ? (ZU(1) << lg_tmin) :
		    (ZU(1) << lg_ceil));
	} else
#endif
	{
		size_t x = lg_floor((size<<1)-1);
		size_t lg_delta = (x < LG_SIZE_CLASS_GROUP + LG_QUANTUM + 1)
		    ?  LG_QUANTUM : x - LG_SIZE_CLASS_GROUP - 1;
		size_t delta = ZU(1) << lg_delta;
		size_t delta_mask = delta - 1;
		size_t usize = (size + delta_mask) & ~delta_mask;
		return (usize);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
small_s2u_lookup(size_t size)
{
	size_t ret = small_bin2size(small_size2bin(size));

	assert(ret == small_s2u_compute(size));
	return (ret);
}

JEMALLOC_ALWAYS_INLINE size_t
small_s2u(size_t size)
{

	assert(size > 0);
	if (likely(size <= LOOKUP_MAXCLASS))
		return (small_s2u_lookup(size));
	else
		return (small_s2u_compute(size));
}
#  endif /* JEMALLOC_ARENA_INLINE_A */

#  ifdef JEMALLOC_ARENA_INLINE_B
JEMALLOC_ALWAYS_INLINE arena_chunk_map_bits_t *
arena_bitselm_get(arena_chunk_t *chunk, size_t pageind)
{

	assert(pageind >= map_bias);
	assert(pageind < chunk_npages);

	return (&chunk->map_bits[pageind-map_bias]);
}

JEMALLOC_ALWAYS_INLINE arena_chunk_map_misc_t *
arena_miscelm_get(arena_chunk_t *chunk, size_t pageind)
{

	assert(pageind >= map_bias);
	assert(pageind < chunk_npages);

	return ((arena_chunk_map_misc_t *)((uintptr_t)chunk +
	    (uintptr_t)map_misc_offset) + pageind-map_bias);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_miscelm_to_pageind(arena_chunk_map_misc_t *miscelm)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
	size_t pageind = ((uintptr_t)miscelm - ((uintptr_t)chunk +
	    map_misc_offset)) / sizeof(arena_chunk_map_misc_t) + map_bias;

	assert(pageind >= map_bias);
	assert(pageind < chunk_npages);

	return (pageind);
}

JEMALLOC_ALWAYS_INLINE void *
arena_miscelm_to_rpages(arena_chunk_map_misc_t *miscelm)
{
	arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(miscelm);
	size_t pageind = arena_miscelm_to_pageind(miscelm);

	return ((void *)((uintptr_t)chunk + (pageind << LG_PAGE)));
}

JEMALLOC_ALWAYS_INLINE arena_chunk_map_misc_t *
arena_run_to_miscelm(arena_run_t *run)
{
	arena_chunk_map_misc_t *miscelm = (arena_chunk_map_misc_t
	    *)((uintptr_t)run - offsetof(arena_chunk_map_misc_t, run));

	assert(arena_miscelm_to_pageind(miscelm) >= map_bias);
	assert(arena_miscelm_to_pageind(miscelm) < chunk_npages);

	return (miscelm);
}

JEMALLOC_ALWAYS_INLINE size_t *
arena_mapbitsp_get(arena_chunk_t *chunk, size_t pageind)
{

	return (&arena_bitselm_get(chunk, pageind)->bits);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbitsp_read(size_t *mapbitsp)
{

	return (*mapbitsp);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_get(arena_chunk_t *chunk, size_t pageind)
{

	return (arena_mapbitsp_read(arena_mapbitsp_get(chunk, pageind)));
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_unallocated_size_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	assert((mapbits & (CHUNK_MAP_LARGE|CHUNK_MAP_ALLOCATED)) == 0);
	return (mapbits & ~PAGE_MASK);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_large_size_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	assert((mapbits & (CHUNK_MAP_LARGE|CHUNK_MAP_ALLOCATED)) ==
	    (CHUNK_MAP_LARGE|CHUNK_MAP_ALLOCATED));
	return (mapbits & ~PAGE_MASK);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_small_runind_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	assert((mapbits & (CHUNK_MAP_LARGE|CHUNK_MAP_ALLOCATED)) ==
	    CHUNK_MAP_ALLOCATED);
	return (mapbits >> LG_PAGE);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_binind_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;
	size_t binind;

	mapbits = arena_mapbits_get(chunk, pageind);
	binind = (mapbits & CHUNK_MAP_BININD_MASK) >> CHUNK_MAP_BININD_SHIFT;
	assert(binind < NBINS || binind == BININD_INVALID);
	return (binind);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_dirty_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	return (mapbits & CHUNK_MAP_DIRTY);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_unzeroed_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	return (mapbits & CHUNK_MAP_UNZEROED);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_large_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	return (mapbits & CHUNK_MAP_LARGE);
}

JEMALLOC_ALWAYS_INLINE size_t
arena_mapbits_allocated_get(arena_chunk_t *chunk, size_t pageind)
{
	size_t mapbits;

	mapbits = arena_mapbits_get(chunk, pageind);
	return (mapbits & CHUNK_MAP_ALLOCATED);
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbitsp_write(size_t *mapbitsp, size_t mapbits)
{

	*mapbitsp = mapbits;
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_unallocated_set(arena_chunk_t *chunk, size_t pageind, size_t size,
    size_t flags)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);

	assert((size & PAGE_MASK) == 0);
	assert((flags & ~CHUNK_MAP_FLAGS_MASK) == 0);
	assert((flags & (CHUNK_MAP_DIRTY|CHUNK_MAP_UNZEROED)) == flags);
	arena_mapbitsp_write(mapbitsp, size | CHUNK_MAP_BININD_INVALID | flags);
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_unallocated_size_set(arena_chunk_t *chunk, size_t pageind,
    size_t size)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);
	size_t mapbits = arena_mapbitsp_read(mapbitsp);

	assert((size & PAGE_MASK) == 0);
	assert((mapbits & (CHUNK_MAP_LARGE|CHUNK_MAP_ALLOCATED)) == 0);
	arena_mapbitsp_write(mapbitsp, size | (mapbits & PAGE_MASK));
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_large_set(arena_chunk_t *chunk, size_t pageind, size_t size,
    size_t flags)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);
	size_t mapbits = arena_mapbitsp_read(mapbitsp);
	size_t unzeroed;

	assert((size & PAGE_MASK) == 0);
	assert((flags & CHUNK_MAP_DIRTY) == flags);
	unzeroed = mapbits & CHUNK_MAP_UNZEROED; /* Preserve unzeroed. */
	arena_mapbitsp_write(mapbitsp, size | CHUNK_MAP_BININD_INVALID | flags
	    | unzeroed | CHUNK_MAP_LARGE | CHUNK_MAP_ALLOCATED);
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_large_binind_set(arena_chunk_t *chunk, size_t pageind,
    size_t binind)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);
	size_t mapbits = arena_mapbitsp_read(mapbitsp);

	assert(binind <= BININD_INVALID);
	assert(arena_mapbits_large_size_get(chunk, pageind) == PAGE);
	arena_mapbitsp_write(mapbitsp, (mapbits & ~CHUNK_MAP_BININD_MASK) |
	    (binind << CHUNK_MAP_BININD_SHIFT));
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_small_set(arena_chunk_t *chunk, size_t pageind, size_t runind,
    size_t binind, size_t flags)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);
	size_t mapbits = arena_mapbitsp_read(mapbitsp);
	size_t unzeroed;

	assert(binind < BININD_INVALID);
	assert(pageind - runind >= map_bias);
	assert((flags & CHUNK_MAP_DIRTY) == flags);
	unzeroed = mapbits & CHUNK_MAP_UNZEROED; /* Preserve unzeroed. */
	arena_mapbitsp_write(mapbitsp, (runind << LG_PAGE) | (binind <<
	    CHUNK_MAP_BININD_SHIFT) | flags | unzeroed | CHUNK_MAP_ALLOCATED);
}

JEMALLOC_ALWAYS_INLINE void
arena_mapbits_unzeroed_set(arena_chunk_t *chunk, size_t pageind,
    size_t unzeroed)
{
	size_t *mapbitsp = arena_mapbitsp_get(chunk, pageind);
	size_t mapbits = arena_mapbitsp_read(mapbitsp);

	arena_mapbitsp_write(mapbitsp, (mapbits & ~CHUNK_MAP_UNZEROED) |
	    unzeroed);
}

JEMALLOC_INLINE bool
arena_prof_accum_impl(arena_t *arena, uint64_t accumbytes)
{

	cassert(config_prof);
	assert(prof_interval != 0);

	arena->prof_accumbytes += accumbytes;
	if (arena->prof_accumbytes >= prof_interval) {
		arena->prof_accumbytes -= prof_interval;
		return (true);
	}
	return (false);
}

JEMALLOC_INLINE bool
arena_prof_accum_locked(arena_t *arena, uint64_t accumbytes)
{

	cassert(config_prof);

	if (likely(prof_interval == 0))
		return (false);
	return (arena_prof_accum_impl(arena, accumbytes));
}

JEMALLOC_INLINE bool
arena_prof_accum(arena_t *arena, uint64_t accumbytes)
{

	cassert(config_prof);

	if (likely(prof_interval == 0))
		return (false);

	{
		bool ret;

		malloc_mutex_lock(&arena->lock);
		ret = arena_prof_accum_impl(arena, accumbytes);
		malloc_mutex_unlock(&arena->lock);
		return (ret);
	}
}

JEMALLOC_ALWAYS_INLINE size_t
arena_ptr_small_binind_get(const void *ptr, size_t mapbits)
{
	size_t binind;

	binind = (mapbits & CHUNK_MAP_BININD_MASK) >> CHUNK_MAP_BININD_SHIFT;

	if (config_debug) {
		arena_chunk_t *chunk;
		arena_t *arena;
		size_t pageind;
		size_t actual_mapbits;
		size_t rpages_ind;
		arena_run_t *run;
		arena_bin_t *bin;
		size_t actual_binind;
		arena_bin_info_t *bin_info;
		arena_chunk_map_misc_t *miscelm;
		void *rpages;

		assert(binind != BININD_INVALID);
		assert(binind < NBINS);
		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
		arena = chunk->arena;
		pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
		actual_mapbits = arena_mapbits_get(chunk, pageind);
		assert(mapbits == actual_mapbits);
		assert(arena_mapbits_large_get(chunk, pageind) == 0);
		assert(arena_mapbits_allocated_get(chunk, pageind) != 0);
		rpages_ind = pageind - arena_mapbits_small_runind_get(chunk,
		    pageind);
		miscelm = arena_miscelm_get(chunk, rpages_ind);
		run = &miscelm->run;
		bin = run->bin;
		actual_binind = bin - arena->bins;
		assert(binind == actual_binind);
		bin_info = &arena_bin_info[actual_binind];
		rpages = arena_miscelm_to_rpages(miscelm);
		assert(((uintptr_t)ptr - ((uintptr_t)rpages +
		    (uintptr_t)bin_info->reg0_offset)) % bin_info->reg_interval
		    == 0);
	}

	return (binind);
}
#  endif /* JEMALLOC_ARENA_INLINE_B */

#  ifdef JEMALLOC_ARENA_INLINE_C
JEMALLOC_INLINE size_t
arena_bin_index(arena_t *arena, arena_bin_t *bin)
{
	size_t binind = bin - arena->bins;
	assert(binind < NBINS);
	return (binind);
}

JEMALLOC_INLINE unsigned
arena_run_regind(arena_run_t *run, arena_bin_info_t *bin_info, const void *ptr)
{
	unsigned shift, diff, regind;
	size_t interval;
	arena_chunk_map_misc_t *miscelm = arena_run_to_miscelm(run);
	void *rpages = arena_miscelm_to_rpages(miscelm);

	/*
	 * Freeing a pointer lower than region zero can cause assertion
	 * failure.
	 */
	assert((uintptr_t)ptr >= (uintptr_t)rpages +
	    (uintptr_t)bin_info->reg0_offset);

	/*
	 * Avoid doing division with a variable divisor if possible.  Using
	 * actual division here can reduce allocator throughput by over 20%!
	 */
	diff = (unsigned)((uintptr_t)ptr - (uintptr_t)rpages -
	    bin_info->reg0_offset);

	/* Rescale (factor powers of 2 out of the numerator and denominator). */
	interval = bin_info->reg_interval;
	shift = jemalloc_ffs(interval) - 1;
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
#define	SIZE_INV_SHIFT	((sizeof(unsigned) << 3) - LG_RUN_MAXREGS)
#define	SIZE_INV(s)	(((1U << SIZE_INV_SHIFT) / (s)) + 1)
		static const unsigned interval_invs[] = {
		    SIZE_INV(3),
		    SIZE_INV(4), SIZE_INV(5), SIZE_INV(6), SIZE_INV(7),
		    SIZE_INV(8), SIZE_INV(9), SIZE_INV(10), SIZE_INV(11),
		    SIZE_INV(12), SIZE_INV(13), SIZE_INV(14), SIZE_INV(15),
		    SIZE_INV(16), SIZE_INV(17), SIZE_INV(18), SIZE_INV(19),
		    SIZE_INV(20), SIZE_INV(21), SIZE_INV(22), SIZE_INV(23),
		    SIZE_INV(24), SIZE_INV(25), SIZE_INV(26), SIZE_INV(27),
		    SIZE_INV(28), SIZE_INV(29), SIZE_INV(30), SIZE_INV(31)
		};

		if (likely(interval <= ((sizeof(interval_invs) /
		    sizeof(unsigned)) + 2))) {
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

JEMALLOC_INLINE prof_tctx_t *
arena_prof_tctx_get(const void *ptr)
{
	prof_tctx_t *ret;
	arena_chunk_t *chunk;
	size_t pageind, mapbits;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	mapbits = arena_mapbits_get(chunk, pageind);
	assert((mapbits & CHUNK_MAP_ALLOCATED) != 0);
	if (likely((mapbits & CHUNK_MAP_LARGE) == 0))
		ret = (prof_tctx_t *)(uintptr_t)1U;
	else
		ret = arena_miscelm_get(chunk, pageind)->prof_tctx;

	return (ret);
}

JEMALLOC_INLINE void
arena_prof_tctx_set(const void *ptr, prof_tctx_t *tctx)
{
	arena_chunk_t *chunk;
	size_t pageind;

	cassert(config_prof);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	assert(arena_mapbits_allocated_get(chunk, pageind) != 0);

	if (unlikely(arena_mapbits_large_get(chunk, pageind) != 0))
		arena_miscelm_get(chunk, pageind)->prof_tctx = tctx;
}

JEMALLOC_ALWAYS_INLINE void *
arena_malloc(tsd_t *tsd, arena_t *arena, size_t size, bool zero,
    bool try_tcache)
{
	tcache_t *tcache;

	assert(size != 0);
	assert(size <= arena_maxclass);

	if (likely(size <= SMALL_MAXCLASS)) {
		if (likely(try_tcache) && likely((tcache = tcache_get(tsd,
		    true)) != NULL))
			return (tcache_alloc_small(tcache, size, zero));
		else {
			return (arena_malloc_small(choose_arena(tsd, arena),
			    size, zero));
		}
	} else {
		/*
		 * Initialize tcache after checking size in order to avoid
		 * infinite recursion during tcache initialization.
		 */
		if (try_tcache && size <= tcache_maxclass && likely((tcache =
		    tcache_get(tsd, true)) != NULL))
			return (tcache_alloc_large(tcache, size, zero));
		else {
			return (arena_malloc_large(choose_arena(tsd, arena),
			    size, zero));
		}
	}
}

/* Return the size of the allocation pointed to by ptr. */
JEMALLOC_ALWAYS_INLINE size_t
arena_salloc(const void *ptr, bool demote)
{
	size_t ret;
	arena_chunk_t *chunk;
	size_t pageind, binind;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	assert(arena_mapbits_allocated_get(chunk, pageind) != 0);
	binind = arena_mapbits_binind_get(chunk, pageind);
	if (unlikely(binind == BININD_INVALID || (config_prof && !demote &&
	    arena_mapbits_large_get(chunk, pageind) != 0))) {
		/*
		 * Large allocation.  In the common case (demote), and as this
		 * is an inline function, most callers will only end up looking
		 * at binind to determine that ptr is a small allocation.
		 */
		assert(((uintptr_t)ptr & PAGE_MASK) == 0);
		ret = arena_mapbits_large_size_get(chunk, pageind);
		assert(ret != 0);
		assert(pageind + (ret>>LG_PAGE) <= chunk_npages);
		assert(ret == PAGE || arena_mapbits_large_size_get(chunk,
		    pageind+(ret>>LG_PAGE)-1) == 0);
		assert(binind == arena_mapbits_binind_get(chunk,
		    pageind+(ret>>LG_PAGE)-1));
		assert(arena_mapbits_dirty_get(chunk, pageind) ==
		    arena_mapbits_dirty_get(chunk, pageind+(ret>>LG_PAGE)-1));
	} else {
		/* Small allocation (possibly promoted to a large object). */
		assert(arena_mapbits_large_get(chunk, pageind) != 0 ||
		    arena_ptr_small_binind_get(ptr, arena_mapbits_get(chunk,
		    pageind)) == binind);
		ret = small_bin2size(binind);
	}

	return (ret);
}

JEMALLOC_ALWAYS_INLINE void
arena_dalloc(tsd_t *tsd, arena_chunk_t *chunk, void *ptr, bool try_tcache)
{
	size_t pageind, mapbits;
	tcache_t *tcache;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
	mapbits = arena_mapbits_get(chunk, pageind);
	assert(arena_mapbits_allocated_get(chunk, pageind) != 0);
	if (likely((mapbits & CHUNK_MAP_LARGE) == 0)) {
		/* Small allocation. */
		if (likely(try_tcache) && likely((tcache = tcache_get(tsd,
		    false)) != NULL)) {
			size_t binind = arena_ptr_small_binind_get(ptr,
			    mapbits);
			tcache_dalloc_small(tcache, ptr, binind);
		} else
			arena_dalloc_small(chunk->arena, chunk, ptr, pageind);
	} else {
		size_t size = arena_mapbits_large_size_get(chunk, pageind);

		assert(((uintptr_t)ptr & PAGE_MASK) == 0);

		if (try_tcache && size <= tcache_maxclass && likely((tcache =
		    tcache_get(tsd, false)) != NULL)) {
			tcache_dalloc_large(tcache, ptr, size);
		} else
			arena_dalloc_large(chunk->arena, chunk, ptr);
	}
}

JEMALLOC_ALWAYS_INLINE void
arena_sdalloc(tsd_t *tsd, arena_chunk_t *chunk, void *ptr, size_t size,
    bool try_tcache)
{
	tcache_t *tcache;

	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	if (likely(size <= SMALL_MAXCLASS)) {
		/* Small allocation. */
		if (likely(try_tcache) && likely((tcache = tcache_get(tsd,
		    false)) != NULL)) {
			size_t binind = small_size2bin(size);
			tcache_dalloc_small(tcache, ptr, binind);
		} else {
			size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >>
			    LG_PAGE;
			arena_dalloc_small(chunk->arena, chunk, ptr, pageind);
		}
	} else {
		assert(((uintptr_t)ptr & PAGE_MASK) == 0);

		if (try_tcache && size <= tcache_maxclass && (tcache =
		    tcache_get(tsd, false)) != NULL) {
			tcache_dalloc_large(tcache, ptr, size);
		} else
			arena_dalloc_large(chunk->arena, chunk, ptr);
	}
}
#  endif /* JEMALLOC_ARENA_INLINE_C */
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
