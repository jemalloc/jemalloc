/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/*
 * Subpages are an artificially designated partitioning of pages.  Their only
 * purpose is to support subpage-spaced size classes.
 *
 * There must be at least 4 subpages per page, due to the way size classes are
 * handled.
 */
#define	LG_SUBPAGE		8
#define	SUBPAGE			((size_t)(1U << LG_SUBPAGE))
#define	SUBPAGE_MASK		(SUBPAGE - 1)

/* Return the smallest subpage multiple that is >= s. */
#define	SUBPAGE_CEILING(s)						\
	(((s) + SUBPAGE_MASK) & ~SUBPAGE_MASK)

#ifdef JEMALLOC_TINY
   /* Smallest size class to support. */
#  define LG_TINY_MIN		LG_SIZEOF_PTR
#endif

/*
 * Maximum size class that is a multiple of the quantum, but not (necessarily)
 * a power of 2.  Above this size, allocations are rounded up to the nearest
 * power of 2.
 */
#define	LG_QSPACE_MAX_DEFAULT	7

/*
 * Maximum size class that is a multiple of the cacheline, but not (necessarily)
 * a power of 2.  Above this size, allocations are rounded up to the nearest
 * power of 2.
 */
#define	LG_CSPACE_MAX_DEFAULT	9

/*
 * RUN_MAX_OVRHD indicates maximum desired run header overhead.  Runs are sized
 * as small as possible such that this setting is still honored, without
 * violating other constraints.  The goal is to make runs as small as possible
 * without exceeding a per run external fragmentation threshold.
 *
 * We use binary fixed point math for overhead computations, where the binary
 * point is implicitly RUN_BFP bits to the left.
 *
 * Note that it is possible to set RUN_MAX_OVRHD low enough that it cannot be
 * honored for some/all object sizes, since there is one bit of header overhead
 * per object (plus a constant).  This constraint is relaxed (ignored) for runs
 * that are so small that the per-region overhead is greater than:
 *
 *   (RUN_MAX_OVRHD / (reg_size << (3+RUN_BFP))
 */
#define	RUN_BFP			12
/*                                    \/   Implicit binary fixed point. */
#define	RUN_MAX_OVRHD		0x0000003dU
#define	RUN_MAX_OVRHD_RELAX	0x00001800U

/*
 * The minimum ratio of active:dirty pages per arena is computed as:
 *
 *   (nactive >> opt_lg_dirty_mult) >= ndirty
 *
 * So, supposing that opt_lg_dirty_mult is 5, there can be no less than 32
 * times as many active pages as dirty pages.
 */
#define	LG_DIRTY_MULT_DEFAULT	5

typedef struct arena_chunk_map_s arena_chunk_map_t;
typedef struct arena_chunk_s arena_chunk_t;
typedef struct arena_run_s arena_run_t;
typedef struct arena_bin_s arena_bin_t;
typedef struct arena_s arena_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* Each element of the chunk map corresponds to one page within the chunk. */
struct arena_chunk_map_s {
	union {
		/*
		 * Linkage for run trees.  There are two disjoint uses:
		 *
		 * 1) arena_t's runs_avail_{clean,dirty} trees.
		 * 2) arena_run_t conceptually uses this linkage for in-use
		 *    non-full runs, rather than directly embedding linkage.
		 */
		rb_node(arena_chunk_map_t)	rb_link;
		/*
		 * List of runs currently in purgatory.  arena_chunk_purge()
		 * temporarily allocates runs that contain dirty pages while
		 * purging, so that other threads cannot use the runs while the
		 * purging thread is operating without the arena lock held.
		 */
		ql_elm(arena_chunk_map_t)	ql_link;
	}				u;

#ifdef JEMALLOC_PROF
	/* Profile counters, used for large object runs. */
	prof_thr_cnt_t			*prof_cnt;
#endif

	/*
	 * Run address (or size) and various flags are stored together.  The bit
	 * layout looks like (assuming 32-bit system):
	 *
	 *   ???????? ???????? ????---- ----dzla
	 *
	 * ? : Unallocated: Run address for first/last pages, unset for internal
	 *                  pages.
	 *     Small: Run page offset.
	 *     Large: Run size for first page, unset for trailing pages.
	 * - : Unused.
	 * d : dirty?
	 * z : zeroed?
	 * l : large?
	 * a : allocated?
	 *
	 * Following are example bit patterns for the three types of runs.
	 *
	 * p : run page offset
	 * s : run size
	 * c : size class (used only if prof_promote is true)
	 * x : don't care
	 * - : 0
	 * + : 1
	 * [DZLA] : bit set
	 * [dzla] : bit unset
	 *
	 *   Unallocated (clean):
	 *     ssssssss ssssssss ssss---- ----dz--
	 *     xxxxxxxx xxxxxxxx xxxx---- -----Zxx
	 *     ssssssss ssssssss ssss---- ----dZ--
	 *
	 *   Unallocated (dirty):
	 *     ssssssss ssssssss ssss---- ----D---
	 *     xxxxxxxx xxxxxxxx xxxx---- ----xxxx
	 *     ssssssss ssssssss ssss---- ----D---
	 *
	 *   Small:
	 *     pppppppp pppppppp pppp---- ----d--a
	 *     pppppppp pppppppp pppp---- -------a
	 *     pppppppp pppppppp pppp---- ----d--a
	 *
	 *   Large:
	 *     ssssssss ssssssss ssss++++ ++++D-la
	 *     xxxxxxxx xxxxxxxx xxxx---- ----xxxx
	 *     -------- -------- -------- ----D-la
	 *
	 *   Large (sampled, size <= PAGE_SIZE):
	 *     ssssssss ssssssss sssscccc ccccD-la
	 *
	 *   Large (not sampled, size == PAGE_SIZE):
	 *     ssssssss ssssssss ssss++++ ++++D-la
	 */
	size_t				bits;
#ifdef JEMALLOC_PROF
#define	CHUNK_MAP_CLASS_SHIFT	4
#define	CHUNK_MAP_CLASS_MASK	((size_t)0xff0U)
#endif
#define	CHUNK_MAP_FLAGS_MASK	((size_t)0xfU)
#define	CHUNK_MAP_DIRTY		((size_t)0x8U)
#define	CHUNK_MAP_ZEROED	((size_t)0x4U)
#define	CHUNK_MAP_LARGE		((size_t)0x2U)
#define	CHUNK_MAP_ALLOCATED	((size_t)0x1U)
#define	CHUNK_MAP_KEY		CHUNK_MAP_ALLOCATED
};
typedef rb_tree(arena_chunk_map_t) arena_avail_tree_t;
typedef rb_tree(arena_chunk_map_t) arena_run_tree_t;

/* Arena chunk header. */
struct arena_chunk_s {
	/* Arena that owns the chunk. */
	arena_t		*arena;

	/* Linkage for the arena's chunks_dirty list. */
	ql_elm(arena_chunk_t) link_dirty;

	/*
	 * True if the chunk is currently in the chunks_dirty list, due to
	 * having at some point contained one or more dirty pages.  Removal
	 * from chunks_dirty is lazy, so (dirtied && ndirty == 0) is possible.
	 */
	bool		dirtied;

	/* Number of dirty pages. */
	size_t		ndirty;

	/* Map of pages within chunk that keeps track of free/large/small. */
	arena_chunk_map_t map[1]; /* Dynamically sized. */
};
typedef rb_tree(arena_chunk_t) arena_chunk_tree_t;

struct arena_run_s {
#ifdef JEMALLOC_DEBUG
	uint32_t	magic;
#  define ARENA_RUN_MAGIC 0x384adf93
#endif

	/* Bin this run is associated with. */
	arena_bin_t	*bin;

	/* Stack of available freed regions, or NULL. */
	void		*avail;

	/* Next region that has never been allocated, or run boundary. */
	void		*next;

	/* Number of free regions in run. */
	unsigned	nfree;
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

	/* Size of regions in a run for this bin's size class. */
	size_t		reg_size;

	/* Total size of a run for this bin's size class. */
	size_t		run_size;

	/* Total number of regions in a run for this bin's size class. */
	uint32_t	nregs;

#ifdef JEMALLOC_PROF
	/*
	 * Offset of first (prof_cnt_t *) in a run header for this bin's size
	 * class, or 0 if (opt_prof == false).
	 */
	uint32_t	cnt0_offset;
#endif

	/* Offset of first region in a run for this bin's size class. */
	uint32_t	reg0_offset;

#ifdef JEMALLOC_STATS
	/* Bin statistics. */
	malloc_bin_stats_t stats;
#endif
};

struct arena_s {
#ifdef JEMALLOC_DEBUG
	uint32_t		magic;
#  define ARENA_MAGIC 0x947d3d24
#endif

	/* This arena's index within the arenas array. */
	unsigned		ind;

	/*
	 * All non-bin-related operations on this arena require that lock be
	 * locked.
	 */
	malloc_mutex_t		lock;

#ifdef JEMALLOC_STATS
	arena_stats_t		stats;
#  ifdef JEMALLOC_TCACHE
	/*
	 * List of tcaches for extant threads associated with this arena.
	 * Stats from these are merged incrementally, and at exit.
	 */
	ql_head(tcache_t)	tcache_ql;
#  endif
#endif

#ifdef JEMALLOC_PROF
	uint64_t		prof_accumbytes;
#endif

	/* List of dirty-page-containing chunks this arena manages. */
	ql_head(arena_chunk_t)	chunks_dirty;

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

	/* Number of pages in active runs. */
	size_t			nactive;

	/*
	 * Current count of pages within unused runs that are potentially
	 * dirty, and for which madvise(... MADV_DONTNEED) has not been called.
	 * By tracking this, we can institute a limit on how much dirty unused
	 * memory is mapped for each arena.
	 */
	size_t			ndirty;

	/*
	 * Approximate number of pages being purged.  It is possible for
	 * multiple threads to purge dirty pages concurrently, and they use
	 * npurgatory to indicate the total number of pages all threads are
	 * attempting to purge.
	 */
	size_t			npurgatory;

	/*
	 * Size/address-ordered trees of this arena's available runs.  The trees
	 * are used for first-best-fit run allocation.  The dirty tree contains
	 * runs with dirty pages (i.e. very likely to have been touched and
	 * therefore have associated physical pages), whereas the clean tree
	 * contains runs with pages that either have no associated physical
	 * pages, or have pages that the kernel may recycle at any time due to
	 * previous madvise(2) calls.  The dirty tree is used in preference to
	 * the clean tree for allocations, because using dirty pages reduces
	 * the amount of dirty purging necessary to keep the active:dirty page
	 * ratio below the purge threshold.
	 */
	arena_avail_tree_t	runs_avail_clean;
	arena_avail_tree_t	runs_avail_dirty;

	/*
	 * bins is used to store trees of free regions of the following sizes,
	 * assuming a 16-byte quantum, 4 KiB page size, and default
	 * JEMALLOC_OPTIONS.
	 *
	 *   bins[i] |   size |
	 *   --------+--------+
	 *        0  |      2 |
	 *        1  |      4 |
	 *        2  |      8 |
	 *   --------+--------+
	 *        3  |     16 |
	 *        4  |     32 |
	 *        5  |     48 |
	 *           :        :
	 *        8  |     96 |
	 *        9  |    112 |
	 *       10  |    128 |
	 *   --------+--------+
	 *       11  |    192 |
	 *       12  |    256 |
	 *       13  |    320 |
	 *       14  |    384 |
	 *       15  |    448 |
	 *       16  |    512 |
	 *   --------+--------+
	 *       17  |    768 |
	 *       18  |   1024 |
	 *       19  |   1280 |
	 *           :        :
	 *       27  |   3328 |
	 *       28  |   3584 |
	 *       29  |   3840 |
	 *   --------+--------+
	 *       30  |  4 KiB |
	 *       31  |  6 KiB |
	 *       33  |  8 KiB |
	 *           :        :
	 *       43  | 28 KiB |
	 *       44  | 30 KiB |
	 *       45  | 32 KiB |
	 *   --------+--------+
	 */
	arena_bin_t		bins[1]; /* Dynamically sized. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern size_t	opt_lg_qspace_max;
extern size_t	opt_lg_cspace_max;
extern ssize_t		opt_lg_dirty_mult;
extern uint8_t const	*small_size2bin;

/* Various bin-related settings. */
#ifdef JEMALLOC_TINY		/* Number of (2^n)-spaced tiny bins. */
#  define		ntbins	((unsigned)(LG_QUANTUM - LG_TINY_MIN))
#else
#  define		ntbins	0
#endif
extern unsigned		nqbins; /* Number of quantum-spaced bins. */
extern unsigned		ncbins; /* Number of cacheline-spaced bins. */
extern unsigned		nsbins; /* Number of subpage-spaced bins. */
extern unsigned		nbins;
#ifdef JEMALLOC_TINY
#  define		tspace_max	((size_t)(QUANTUM >> 1))
#endif
#define			qspace_min	QUANTUM
extern size_t		qspace_max;
extern size_t		cspace_min;
extern size_t		cspace_max;
extern size_t		sspace_min;
extern size_t		sspace_max;
#define			small_maxclass	sspace_max

#define			nlclasses (chunk_npages - arena_chunk_header_npages)

#ifdef JEMALLOC_TCACHE
void	arena_tcache_fill_small(arena_t *arena, tcache_bin_t *tbin,
    size_t binind
#  ifdef JEMALLOC_PROF
    , uint64_t prof_accumbytes
#  endif
    );
#endif
#ifdef JEMALLOC_PROF
void	arena_prof_accum(arena_t *arena, uint64_t accumbytes);
#endif
void	*arena_malloc_small(arena_t *arena, size_t size, bool zero);
void	*arena_malloc_large(arena_t *arena, size_t size, bool zero);
void	*arena_malloc(size_t size, bool zero);
void	*arena_palloc(arena_t *arena, size_t alignment, size_t size,
    size_t alloc_size);
size_t	arena_salloc(const void *ptr);
#ifdef JEMALLOC_PROF
void	arena_prof_promoted(const void *ptr, size_t size);
size_t	arena_salloc_demote(const void *ptr);
prof_thr_cnt_t	*arena_prof_cnt_get(const void *ptr);
void	arena_prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt);
#endif
void	arena_dalloc_bin(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_t *mapelm);
void	arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk, void *ptr);
#ifdef JEMALLOC_STATS
void	arena_stats_merge(arena_t *arena, size_t *nactive, size_t *ndirty,
    arena_stats_t *astats, malloc_bin_stats_t *bstats,
    malloc_large_stats_t *lstats);
#endif
void	*arena_ralloc(void *ptr, size_t size, size_t oldsize);
bool	arena_new(arena_t *arena, unsigned ind);
bool	arena_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	arena_dalloc(arena_t *arena, arena_chunk_t *chunk, void *ptr);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_ARENA_C_))
JEMALLOC_INLINE void
arena_dalloc(arena_t *arena, arena_chunk_t *chunk, void *ptr)
{
	size_t pageind;
	arena_chunk_map_t *mapelm;

	assert(arena != NULL);
	assert(arena->magic == ARENA_MAGIC);
	assert(chunk->arena == arena);
	assert(ptr != NULL);
	assert(CHUNK_ADDR2BASE(ptr) != ptr);

	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];
	assert((mapelm->bits & CHUNK_MAP_ALLOCATED) != 0);
	if ((mapelm->bits & CHUNK_MAP_LARGE) == 0) {
		/* Small allocation. */
#ifdef JEMALLOC_TCACHE
		tcache_t *tcache;

		if ((tcache = tcache_get()) != NULL)
			tcache_dalloc_small(tcache, ptr);
		else {
#endif
			arena_run_t *run;
			arena_bin_t *bin;

			run = (arena_run_t *)((uintptr_t)chunk +
			    (uintptr_t)((pageind - (mapelm->bits >>
			    PAGE_SHIFT)) << PAGE_SHIFT));
			assert(run->magic == ARENA_RUN_MAGIC);
			assert(((uintptr_t)ptr - ((uintptr_t)run +
			    (uintptr_t)run->bin->reg0_offset)) %
			    run->bin->reg_size == 0);
			bin = run->bin;
			malloc_mutex_lock(&bin->lock);
			arena_dalloc_bin(arena, chunk, ptr, mapelm);
			malloc_mutex_unlock(&bin->lock);
#ifdef JEMALLOC_TCACHE
		}
#endif
	} else {
#ifdef JEMALLOC_TCACHE
		size_t size = mapelm->bits & ~PAGE_MASK;

		assert(((uintptr_t)ptr & PAGE_MASK) == 0);
		if (size <= tcache_maxclass) {
			tcache_t *tcache;

			if ((tcache = tcache_get()) != NULL)
				tcache_dalloc_large(tcache, ptr, size);
			else {
				malloc_mutex_lock(&arena->lock);
				arena_dalloc_large(arena, chunk, ptr);
				malloc_mutex_unlock(&arena->lock);
			}
		} else {
			malloc_mutex_lock(&arena->lock);
			arena_dalloc_large(arena, chunk, ptr);
			malloc_mutex_unlock(&arena->lock);
		}
#else
		assert(((uintptr_t)ptr & PAGE_MASK) == 0);
		malloc_mutex_lock(&arena->lock);
		arena_dalloc_large(arena, chunk, ptr);
		malloc_mutex_unlock(&arena->lock);
#endif
	}
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
