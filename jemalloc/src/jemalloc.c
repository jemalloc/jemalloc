/*-
 * This allocator implementation is designed to provide scalable performance
 * for multi-threaded programs on multi-processor systems.  The following
 * features are included for this purpose:
 *
 *   + Multiple arenas are used if there are multiple CPUs, which reduces lock
 *     contention and cache sloshing.
 *
 *   + Thread-specific caching is used if there are multiple threads, which
 *     reduces the amount of locking.
 *
 *   + Cache line sharing between arenas is avoided for internal data
 *     structures.
 *
 *   + Memory is managed in chunks and runs (chunks can be split into runs),
 *     rather than as individual pages.  This provides a constant-time
 *     mechanism for associating allocations with particular arenas.
 *
 * Allocation requests are rounded up to the nearest size class, and no record
 * of the original request size is maintained.  Allocations are broken into
 * categories according to size class.  Assuming runtime defaults, 4 KiB pages
 * and a 16 byte quantum on a 32-bit system, the size classes in each category
 * are as follows:
 *
 *   |========================================|
 *   | Category | Subcategory      |     Size |
 *   |========================================|
 *   | Small    | Tiny             |        2 |
 *   |          |                  |        4 |
 *   |          |                  |        8 |
 *   |          |------------------+----------|
 *   |          | Quantum-spaced   |       16 |
 *   |          |                  |       32 |
 *   |          |                  |       48 |
 *   |          |                  |      ... |
 *   |          |                  |       96 |
 *   |          |                  |      112 |
 *   |          |                  |      128 |
 *   |          |------------------+----------|
 *   |          | Cacheline-spaced |      192 |
 *   |          |                  |      256 |
 *   |          |                  |      320 |
 *   |          |                  |      384 |
 *   |          |                  |      448 |
 *   |          |                  |      512 |
 *   |          |------------------+----------|
 *   |          | Sub-page         |      760 |
 *   |          |                  |     1024 |
 *   |          |                  |     1280 |
 *   |          |                  |      ... |
 *   |          |                  |     3328 |
 *   |          |                  |     3584 |
 *   |          |                  |     3840 |
 *   |========================================|
 *   | Medium                      |    4 KiB |
 *   |                             |    6 KiB |
 *   |                             |    8 KiB |
 *   |                             |      ... |
 *   |                             |   28 KiB |
 *   |                             |   30 KiB |
 *   |                             |   32 KiB |
 *   |========================================|
 *   | Large                       |   36 KiB |
 *   |                             |   40 KiB |
 *   |                             |   44 KiB |
 *   |                             |      ... |
 *   |                             | 1012 KiB |
 *   |                             | 1016 KiB |
 *   |                             | 1020 KiB |
 *   |========================================|
 *   | Huge                        |    1 MiB |
 *   |                             |    2 MiB |
 *   |                             |    3 MiB |
 *   |                             |      ... |
 *   |========================================|
 *
 * Different mechanisms are used accoding to category:
 *
 *   Small/medium : Each size class is segregated into its own set of runs.
 *                  Each run maintains a bitmap of which regions are
 *                  free/allocated.
 *
 *   Large : Each allocation is backed by a dedicated run.  Metadata are stored
 *           in the associated arena chunk header maps.
 *
 *   Huge : Each allocation is backed by a dedicated contiguous set of chunks.
 *          Metadata are stored in a separate red-black tree.
 *
 *******************************************************************************
 */

#if 0
__FBSDID("$FreeBSD: src/lib/libc/stdlib/malloc.c,v 1.183 2008/12/01 10:20:59 jasone Exp $");
#endif

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <errno.h>
#include <limits.h>
#ifndef SIZE_T_MAX
#  define SIZE_T_MAX	SIZE_MAX
#endif
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define	JEMALLOC_MANGLE
#include "jemalloc.h"

#ifdef JEMALLOC_LAZY_LOCK
#include <dlfcn.h>
#endif

#ifndef __DECONST
#  define	__DECONST(type, var)	((type)(uintptr_t)(const void *)(var))
#endif

#include "rb.h"
#if (defined(JEMALLOC_TCACHE) && defined(JEMALLOC_STATS))
#include "qr.h"
#include "ql.h"
#endif

#ifdef JEMALLOC_DEBUG
   /* Disable inlining to make debugging easier. */
#  define inline
#endif

/* Size of stack-allocated buffer passed to strerror_r(). */
#define	STRERROR_BUF		64

/*
 * Minimum alignment of allocations is 2^LG_QUANTUM bytes.
 */
#ifdef __i386__
#  define LG_QUANTUM		4
#endif
#ifdef __ia64__
#  define LG_QUANTUM		4
#endif
#ifdef __alpha__
#  define LG_QUANTUM		4
#endif
#ifdef __sparc__
#  define LG_QUANTUM		4
#endif
#ifdef __amd64__
#  define LG_QUANTUM		4
#endif
#ifdef __arm__
#  define LG_QUANTUM		3
#endif
#ifdef __mips__
#  define LG_QUANTUM		3
#endif
#ifdef __powerpc__
#  define LG_QUANTUM		4
#endif
#ifdef __s390x__
#  define LG_QUANTUM		4
#endif

#define	QUANTUM			((size_t)(1U << LG_QUANTUM))
#define	QUANTUM_MASK		(QUANTUM - 1)

#define	SIZEOF_PTR		(1U << LG_SIZEOF_PTR)

/* We can't use TLS in non-PIC programs, since TLS relies on loader magic. */
#if (!defined(PIC) && !defined(NO_TLS))
#  define NO_TLS
#endif

/*
 * Size and alignment of memory chunks that are allocated by the OS's virtual
 * memory system.
 */
#define	LG_CHUNK_DEFAULT	22

/*
 * The minimum ratio of active:dirty pages per arena is computed as:
 *
 *   (nactive >> opt_lg_dirty_mult) >= ndirty
 *
 * So, supposing that opt_lg_dirty_mult is 5, there can be no less than 32
 * times as many active pages as dirty pages.
 */
#define	LG_DIRTY_MULT_DEFAULT	5

/*
 * Maximum size of L1 cache line.  This is used to avoid cache line aliasing.
 * In addition, this controls the spacing of cacheline-spaced size classes.
 */
#define	LG_CACHELINE		6
#define	CACHELINE		((size_t)(1U << LG_CACHELINE))
#define	CACHELINE_MASK		(CACHELINE - 1)

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

#ifdef JEMALLOC_TINY
   /* Smallest size class to support. */
#  define LG_TINY_MIN		1
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
 * Maximum medium size class.  This must not be more than 1/4 of a chunk
 * (LG_MEDIUM_MAX_DEFAULT <= LG_CHUNK_DEFAULT - 2).
 */
#define	LG_MEDIUM_MAX_DEFAULT	15

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

/* Put a cap on small object run size.  This overrides RUN_MAX_OVRHD. */
#define	RUN_MAX_SMALL							\
	(arena_maxclass <= (1U << (CHUNK_MAP_LG_PG_RANGE + PAGE_SHIFT))	\
	    ? arena_maxclass : (1U << (CHUNK_MAP_LG_PG_RANGE +		\
	    PAGE_SHIFT)))

#ifdef JEMALLOC_TCACHE
   /* Number of cache slots for each bin in the thread cache. */
#  define TCACHE_LG_NSLOTS	7
#  define TCACHE_NSLOTS		(1U << TCACHE_LG_NSLOTS)
   /*
    * (1U << opt_lg_tcache_gc_sweep) is the approximate number of
    * allocation events between full GC sweeps (-1: disabled).  Integer
    * rounding may cause the actual number to be slightly higher, since GC is
    * performed incrementally.
    */
#  define LG_TCACHE_GC_SWEEP_DEFAULT	13
#endif

/******************************************************************************/

typedef pthread_mutex_t malloc_mutex_t;

/* Set to true once the allocator has been initialized. */
static bool malloc_initialized = false;

/* Used to let the initializing thread recursively allocate. */
static pthread_t malloc_initializer = (unsigned long)0;

/* Used to avoid initialization races. */
static malloc_mutex_t init_lock = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

/******************************************************************************/
/*
 * Statistics data structures.
 */

#ifdef JEMALLOC_STATS

#ifdef JEMALLOC_TCACHE
typedef struct tcache_bin_stats_s tcache_bin_stats_t;
struct tcache_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t	nrequests;
};
#endif

typedef struct malloc_bin_stats_s malloc_bin_stats_t;
struct malloc_bin_stats_s {
	/*
	 * Number of allocation requests that corresponded to the size of this
	 * bin.
	 */
	uint64_t	nrequests;

#ifdef JEMALLOC_TCACHE
	/* Number of tcache fills from this bin. */
	uint64_t	nfills;

	/* Number of tcache flushes to this bin. */
	uint64_t	nflushes;
#endif

	/* Total number of runs created for this bin's size class. */
	uint64_t	nruns;

	/*
	 * Total number of runs reused by extracting them from the runs tree for
	 * this bin's size class.
	 */
	uint64_t	reruns;

	/* High-water mark for this bin. */
	size_t		highruns;

	/* Current number of runs in this bin. */
	size_t		curruns;
};

typedef struct malloc_large_stats_s malloc_large_stats_t;
struct malloc_large_stats_s {
	/*
	 * Number of allocation requests that corresponded to this size class.
	 */
	uint64_t	nrequests;

	/* High-water mark for this size class. */
	size_t		highruns;

	/* Current number of runs of this size class. */
	size_t		curruns;
};

typedef struct arena_stats_s arena_stats_t;
struct arena_stats_s {
	/* Number of bytes currently mapped. */
	size_t		mapped;

	/*
	 * Total number of purge sweeps, total number of madvise calls made,
	 * and total pages purged in order to keep dirty unused memory under
	 * control.
	 */
	uint64_t	npurge;
	uint64_t	nmadvise;
	uint64_t	purged;

	/* Per-size-category statistics. */
	size_t		allocated_small;
	uint64_t	nmalloc_small;
	uint64_t	ndalloc_small;

	size_t		allocated_medium;
	uint64_t	nmalloc_medium;
	uint64_t	ndalloc_medium;

	size_t		allocated_large;
	uint64_t	nmalloc_large;
	uint64_t	ndalloc_large;

	/*
	 * One element for each possible size class, including sizes that
	 * overlap with bin size classes.  This is necessary because ipalloc()
	 * sometimes has to use such large objects in order to assure proper
	 * alignment.
	 */
	malloc_large_stats_t	*lstats;
};

typedef struct chunk_stats_s chunk_stats_t;
struct chunk_stats_s {
	/* Number of chunks that were allocated. */
	uint64_t	nchunks;

	/* High-water mark for number of chunks allocated. */
	unsigned long	highchunks;

	/*
	 * Current number of chunks allocated.  This value isn't maintained for
	 * any other purpose, so keep track of it in order to be able to set
	 * highchunks.
	 */
	unsigned long	curchunks;
};

#endif /* #ifdef JEMALLOC_STATS */

/******************************************************************************/
/*
 * Extent data structures.
 */

/* Tree of extents. */
typedef struct extent_node_s extent_node_t;
struct extent_node_s {
#ifdef JEMALLOC_DSS
	/* Linkage for the size/address-ordered tree. */
	rb_node(extent_node_t) link_szad;
#endif

	/* Linkage for the address-ordered tree. */
	rb_node(extent_node_t) link_ad;

	/* Pointer to the extent that this tree node is responsible for. */
	void	*addr;

	/* Total region size. */
	size_t	size;
};
typedef rb_tree(extent_node_t) extent_tree_t;

/******************************************************************************/
/*
 * Arena data structures.
 */

typedef struct arena_s arena_t;
typedef struct arena_bin_s arena_bin_t;

/* Each element of the chunk map corresponds to one page within the chunk. */
typedef struct arena_chunk_map_s arena_chunk_map_t;
struct arena_chunk_map_s {
	/*
	 * Linkage for run trees.  There are two disjoint uses:
	 *
	 * 1) arena_t's runs_avail tree.
	 * 2) arena_run_t conceptually uses this linkage for in-use non-full
	 *    runs, rather than directly embedding linkage.
	 */
	rb_node(arena_chunk_map_t)	link;

	/*
	 * Run address (or size) and various flags are stored together.  The bit
	 * layout looks like (assuming 32-bit system):
	 *
	 *   ???????? ???????? ????cccc ccccdzla
	 *
	 * ? : Unallocated: Run address for first/last pages, unset for internal
	 *                  pages.
	 *     Small/medium: Don't care.
	 *     Large: Run size for first page, unset for trailing pages.
	 * - : Unused.
	 * c : refcount (could overflow for PAGE_SIZE >= 128 KiB)
	 * d : dirty?
	 * z : zeroed?
	 * l : large?
	 * a : allocated?
	 *
	 * Following are example bit patterns for the three types of runs.
	 *
	 * p : run page offset
	 * s : run size
	 * x : don't care
	 * - : 0
	 * [dzla] : bit set
	 *
	 *   Unallocated:
	 *     ssssssss ssssssss ssss---- --------
	 *     xxxxxxxx xxxxxxxx xxxx---- ----d---
	 *     ssssssss ssssssss ssss---- -----z--
	 *
	 *   Small/medium:
	 *     pppppppp ppppcccc cccccccc cccc---a
	 *     pppppppp ppppcccc cccccccc cccc---a
	 *     pppppppp ppppcccc cccccccc cccc---a
	 *
	 *   Large:
	 *     ssssssss ssssssss ssss---- ------la
	 *     -------- -------- -------- ------la
	 *     -------- -------- -------- ------la
	 */
	size_t				bits;
#define	CHUNK_MAP_PG_MASK	((size_t)0xfff00000U)
#define	CHUNK_MAP_PG_SHIFT	20
#define	CHUNK_MAP_LG_PG_RANGE	12

#define	CHUNK_MAP_RC_MASK	((size_t)0xffff0U)
#define	CHUNK_MAP_RC_ONE	((size_t)0x00010U)

#define	CHUNK_MAP_FLAGS_MASK	((size_t)0xfU)
#define	CHUNK_MAP_DIRTY		((size_t)0x8U)
#define	CHUNK_MAP_ZEROED	((size_t)0x4U)
#define	CHUNK_MAP_LARGE		((size_t)0x2U)
#define	CHUNK_MAP_ALLOCATED	((size_t)0x1U)
#define	CHUNK_MAP_KEY		(CHUNK_MAP_DIRTY | CHUNK_MAP_ALLOCATED)
};
typedef rb_tree(arena_chunk_map_t) arena_avail_tree_t;
typedef rb_tree(arena_chunk_map_t) arena_run_tree_t;

/* Arena chunk header. */
typedef struct arena_chunk_s arena_chunk_t;
struct arena_chunk_s {
	/* Arena that owns the chunk. */
	arena_t		*arena;

	/* Linkage for the arena's chunks_dirty tree. */
	rb_node(arena_chunk_t) link_dirty;

	/*
	 * True if the chunk is currently in the chunks_dirty tree, due to
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

typedef struct arena_run_s arena_run_t;
struct arena_run_s {
#ifdef JEMALLOC_DEBUG
	uint32_t	magic;
#  define ARENA_RUN_MAGIC 0x384adf93
#endif

	/* Bin this run is associated with. */
	arena_bin_t	*bin;

	/* Index of first element that might have a free region. */
	unsigned	regs_minelm;

	/* Number of free regions in run. */
	unsigned	nfree;

	/* Bitmask of in-use regions (0: in use, 1: free). */
	unsigned	regs_mask[1]; /* Dynamically sized. */
};

struct arena_bin_s {
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

	/* Number of elements in a run's regs_mask for this bin's size class. */
	uint32_t	regs_mask_nelms;

	/* Offset of first region in a run for this bin's size class. */
	uint32_t	reg0_offset;

#ifdef JEMALLOC_STATS
	/* Bin statistics. */
	malloc_bin_stats_t stats;
#endif
};

#ifdef JEMALLOC_TCACHE
typedef struct tcache_s tcache_t;
#endif

struct arena_s {
#ifdef JEMALLOC_DEBUG
	uint32_t		magic;
#  define ARENA_MAGIC 0x947d3d24
#endif

	/* All operations on this arena require that lock be locked. */
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

#ifdef JEMALLOC_TRACE
#  define TRACE_BUF_SIZE 65536
	unsigned		trace_buf_end;
	char			trace_buf[TRACE_BUF_SIZE];
	int			trace_fd;
#endif

	/* Tree of dirty-page-containing chunks this arena manages. */
	arena_chunk_tree_t	chunks_dirty;

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
	size_t		nactive;

	/*
	 * Current count of pages within unused runs that are potentially
	 * dirty, and for which madvise(... MADV_DONTNEED) has not been called.
	 * By tracking this, we can institute a limit on how much dirty unused
	 * memory is mapped for each arena.
	 */
	size_t			ndirty;


	/*
	 * Size/address-ordered tree of this arena's available runs.  This tree
	 * is used for first-best-fit run allocation.
	 */
	arena_avail_tree_t	runs_avail;

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

/******************************************************************************/
/*
 * Thread cache data structures.
 */

#ifdef JEMALLOC_TCACHE
typedef struct tcache_bin_s tcache_bin_t;
struct tcache_bin_s {
#  ifdef JEMALLOC_STATS
	tcache_bin_stats_t tstats;
#  endif
	unsigned	low_water;	/* Min # cached since last GC. */
	unsigned	high_water;	/* Max # cached since last GC. */
	unsigned	ncached;	/* # of cached objects. */
	void		*slots[TCACHE_NSLOTS];
};

struct tcache_s {
#  ifdef JEMALLOC_STATS
	ql_elm(tcache_t) link;		/* Used for aggregating stats. */
#  endif
	arena_t		*arena;		/* This thread's arena. */
	unsigned	ev_cnt;		/* Event count since incremental GC. */
	unsigned	next_gc_bin;	/* Next bin to GC. */
	tcache_bin_t	*tbins[1];	/* Dynamically sized. */
};
#endif

/******************************************************************************/
/*
 * Data.
 */

#ifdef JEMALLOC_LAZY_LOCK
static bool isthreaded = false;
#else
#  define isthreaded true
#endif

/* Number of CPUs. */
static unsigned		ncpus;

#ifdef JEMALLOC_TRACE
static malloc_mutex_t		trace_mtx;
static unsigned			trace_next_tid = 1;

static unsigned __thread	trace_tid
    JEMALLOC_ATTR(tls_model("initial-exec"));
/* Used to cause trace_cleanup() to be called. */
static pthread_key_t		trace_tsd;
#endif

/*
 * Page size.  STATIC_PAGE_SHIFT is determined by the configure script.  If
 * DYNAMIC_PAGE_SHIFT is enabled, only use the STATIC_PAGE_* macros where
 * compile-time values are required for the purposes of defining data
 * structures.
 */
#define	STATIC_PAGE_SIZE ((size_t)(1U << STATIC_PAGE_SHIFT))
#define	STATIC_PAGE_MASK ((size_t)(STATIC_PAGE_SIZE - 1))

#ifdef DYNAMIC_PAGE_SHIFT
static size_t		pagesize;
static size_t		pagesize_mask;
static size_t		lg_pagesize;
#  define PAGE_SHIFT	lg_pagesize
#  define PAGE_SIZE	pagesize
#  define PAGE_MASK	pagesize_mask
#else
#  define PAGE_SHIFT	STATIC_PAGE_SHIFT
#  define PAGE_SIZE	STATIC_PAGE_SIZE
#  define PAGE_MASK	STATIC_PAGE_MASK
#endif

/* Various bin-related settings. */
#ifdef JEMALLOC_TINY		/* Number of (2^n)-spaced tiny bins. */
#  define		ntbins	((unsigned)(LG_QUANTUM - LG_TINY_MIN))
#else
#  define		ntbins	0
#endif
static unsigned		nqbins; /* Number of quantum-spaced bins. */
static unsigned		ncbins; /* Number of cacheline-spaced bins. */
static unsigned		nsbins; /* Number of subpage-spaced bins. */
static unsigned		nmbins; /* Number of medium bins. */
static unsigned		nbins;
static unsigned		mbin0; /* mbin offset (nbins - nmbins). */
#ifdef JEMALLOC_TINY
#  define		tspace_max	((size_t)(QUANTUM >> 1))
#endif
#define			qspace_min	QUANTUM
static size_t		qspace_max;
static size_t		cspace_min;
static size_t		cspace_max;
static size_t		sspace_min;
static size_t		sspace_max;
#define			small_maxclass	sspace_max
#define			medium_min	PAGE_SIZE
static size_t		medium_max;
#define			bin_maxclass	medium_max

/*
 * Soft limit on the number of medium size classes.  Spacing between medium
 * size classes never exceeds pagesize, which can force more than NBINS_MAX
 * medium size classes.
 */
#define	NMBINS_MAX	16
/* Spacing between medium size classes. */
static size_t		lg_mspace;
static size_t		mspace_mask;

static uint8_t const	*small_size2bin;
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

/* Various chunk-related settings. */
static size_t		chunksize;
static size_t		chunksize_mask; /* (chunksize - 1). */
static size_t		chunk_npages;
static size_t		arena_chunk_header_npages;
static size_t		arena_maxclass; /* Max size class for arenas. */

/********/
/*
 * Chunks.
 */

/* Protects chunk-related data structures. */
static malloc_mutex_t	huge_mtx;

/* Tree of chunks that are stand-alone huge allocations. */
static extent_tree_t	huge;

#ifdef JEMALLOC_DSS
/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
static malloc_mutex_t	dss_mtx;
/* Base address of the DSS. */
static void		*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
static void		*dss_prev;
/* Current upper limit on DSS addresses. */
static void		*dss_max;

/*
 * Trees of chunks that were previously allocated (trees differ only in node
 * ordering).  These are used when allocating chunks, in an attempt to re-use
 * address space.  Depending on function, different tree orderings are needed,
 * which is why there are two trees with the same contents.
 */
static extent_tree_t	dss_chunks_szad;
static extent_tree_t	dss_chunks_ad;
#endif

#ifdef JEMALLOC_STATS
/* Huge allocation statistics. */
static uint64_t		huge_nmalloc;
static uint64_t		huge_ndalloc;
static size_t		huge_allocated;
#endif

/****************************/
/*
 * base (internal allocation).
 */

/*
 * Current pages that are being used for internal memory allocations.  These
 * pages are carved up in cacheline-size quanta, so that there is no chance of
 * false cache line sharing.
 */
static void		*base_pages;
static void		*base_next_addr;
static void		*base_past_addr; /* Addr immediately past base_pages. */
static extent_node_t	*base_nodes;
static malloc_mutex_t	base_mtx;
#ifdef JEMALLOC_STATS
static size_t		base_mapped;
#endif

/********/
/*
 * Arenas.
 */

/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 */
static arena_t		**arenas;
static unsigned		narenas;
#ifndef NO_TLS
static unsigned		next_arena;
#endif
static malloc_mutex_t	arenas_lock; /* Protects arenas initialization. */

#ifndef NO_TLS
/*
 * Map of pthread_self() --> arenas[???], used for selecting an arena to use
 * for allocations.
 */
static __thread arena_t		*arenas_map
    JEMALLOC_ATTR(tls_model("initial-exec"));
#endif

#ifdef JEMALLOC_TCACHE
/* Map of thread-specific caches. */
static __thread tcache_t	*tcache_tls
    JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Same contents as tcache, but initialized such that the TSD destructor is
 * called when a thread exits, so that the cache can be cleaned up.
 */
static pthread_key_t		tcache_tsd;

/* Number of tcache allocation/deallocation events between incremental GCs. */
unsigned			tcache_gc_incr;
#endif

/*
 * Used by chunk_alloc_mmap() to decide whether to attempt the fast path and
 * potentially avoid some system calls.  We can get away without TLS here,
 * since the state of mmap_unaligned only affects performance, rather than
 * correct function.
 */
static
#ifndef NO_TLS
       __thread
#endif
                bool	mmap_unaligned
#ifndef NO_TLS
                                       JEMALLOC_ATTR(tls_model("initial-exec"))
#endif
                                                                               ;
#ifdef JEMALLOC_STATS
/* Chunk statistics. */
static chunk_stats_t	stats_chunks;
#endif

/*******************************/
/*
 * Runtime configuration options.
 */
const char	*malloc_options;

#ifdef JEMALLOC_DEBUG
static bool	opt_abort = true;
#  ifdef JEMALLOC_FILL
static bool	opt_junk = true;
#  endif
#else
static bool	opt_abort = false;
#  ifdef JEMALLOC_FILL
static bool	opt_junk = false;
#  endif
#endif
#ifdef JEMALLOC_TCACHE
static bool	opt_tcache = true;
static ssize_t	opt_lg_tcache_gc_sweep = LG_TCACHE_GC_SWEEP_DEFAULT;
#endif
static ssize_t	opt_lg_dirty_mult = LG_DIRTY_MULT_DEFAULT;
static bool	opt_stats_print = false;
static size_t	opt_lg_qspace_max = LG_QSPACE_MAX_DEFAULT;
static size_t	opt_lg_cspace_max = LG_CSPACE_MAX_DEFAULT;
static size_t	opt_lg_medium_max = LG_MEDIUM_MAX_DEFAULT;
static size_t	opt_lg_chunk = LG_CHUNK_DEFAULT;
#ifdef JEMALLOC_TRACE
static bool	opt_trace = false;
#endif
#ifdef JEMALLOC_SYSV
static bool	opt_sysv = false;
#endif
#ifdef JEMALLOC_XMALLOC
static bool	opt_xmalloc = false;
#endif
#ifdef JEMALLOC_FILL
static bool	opt_zero = false;
#endif
static int	opt_narenas_lshift = 0;

/******************************************************************************/
/*
 * Begin function prototypes for non-inline static functions.
 */

static bool	malloc_mutex_init(malloc_mutex_t *mutex);
#ifdef JEMALLOC_TINY
static size_t	pow2_ceil(size_t x);
#endif
static void	wrtmessage(const char *p1, const char *p2, const char *p3,
		const char *p4);
#ifdef JEMALLOC_STATS
static void	malloc_printf(const char *format, ...);
#endif
#ifdef JEMALLOC_LAZY_LOCK
static void	pthread_create_once(void);
#endif
static char	*umax2s(uintmax_t x, unsigned base, char *s);
#ifdef JEMALLOC_DSS
static bool	base_pages_alloc_dss(size_t minsize);
#endif
static bool	base_pages_alloc_mmap(size_t minsize);
static bool	base_pages_alloc(size_t minsize);
static void	*base_alloc(size_t size);
static extent_node_t *base_node_alloc(void);
static void	base_node_dealloc(extent_node_t *node);
static void	*pages_map(void *addr, size_t size);
static void	pages_unmap(void *addr, size_t size);
#ifdef JEMALLOC_DSS
static void	*chunk_alloc_dss(size_t size);
static void	*chunk_recycle_dss(size_t size, bool zero);
#endif
static void	*chunk_alloc_mmap_slow(size_t size, bool unaligned);
static void	*chunk_alloc_mmap(size_t size);
static void	*chunk_alloc(size_t size, bool zero);
#ifdef JEMALLOC_DSS
static extent_node_t *chunk_dealloc_dss_record(void *chunk, size_t size);
static bool	chunk_dealloc_dss(void *chunk, size_t size);
#endif
static void	chunk_dealloc_mmap(void *chunk, size_t size);
static void	chunk_dealloc(void *chunk, size_t size);
#ifndef NO_TLS
static arena_t	*choose_arena_hard(void);
#endif
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
#ifdef JEMALLOC_TCACHE
static void	tcache_bin_fill(tcache_t *tcache, tcache_bin_t *tbin,
    size_t binind);
static void	*tcache_alloc_hard(tcache_t *tcache, tcache_bin_t *tbin,
    size_t binind);
#endif
static void	*arena_malloc_medium(arena_t *arena, size_t size, bool zero);
static void	*arena_malloc_large(arena_t *arena, size_t size, bool zero);
static void	*arena_palloc(arena_t *arena, size_t alignment, size_t size,
    size_t alloc_size);
static bool	arena_is_large(const void *ptr);
static size_t	arena_salloc(const void *ptr);
static void
arena_dalloc_bin_run(arena_t *arena, arena_chunk_t *chunk, arena_run_t *run,
    arena_bin_t *bin);
#ifdef JEMALLOC_STATS
static void	arena_stats_print(arena_t *arena, bool bins, bool large);
#endif
static void	stats_print_atexit(void);
#ifdef JEMALLOC_TCACHE
static void	tcache_bin_flush(tcache_bin_t *tbin, size_t binind,
    unsigned rem);
#endif
static void	arena_dalloc_large(arena_t *arena, arena_chunk_t *chunk,
    void *ptr);
#ifdef JEMALLOC_TCACHE
static void	arena_dalloc_hard(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, arena_chunk_map_t *mapelm, tcache_t *tcache);
#endif
static void	arena_ralloc_large_shrink(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t size, size_t oldsize);
static bool	arena_ralloc_large_grow(arena_t *arena, arena_chunk_t *chunk,
    void *ptr, size_t size, size_t oldsize);
static bool	arena_ralloc_large(void *ptr, size_t size, size_t oldsize);
static void	*arena_ralloc(void *ptr, size_t size, size_t oldsize);
static bool	arena_new(arena_t *arena, unsigned ind);
static arena_t	*arenas_extend(unsigned ind);
#ifdef JEMALLOC_TCACHE
static tcache_bin_t	*tcache_bin_create(arena_t *arena);
static void	tcache_bin_destroy(tcache_t *tcache, tcache_bin_t *tbin,
    unsigned binind);
#  ifdef JEMALLOC_STATS
static void	tcache_stats_merge(tcache_t *tcache, arena_t *arena);
#  endif
static tcache_t	*tcache_create(arena_t *arena);
static void	tcache_destroy(tcache_t *tcache);
static void	tcache_thread_cleanup(void *arg);
#endif
static void	*huge_malloc(size_t size, bool zero);
static void	*huge_palloc(size_t alignment, size_t size);
static void	*huge_ralloc(void *ptr, size_t size, size_t oldsize);
static void	huge_dalloc(void *ptr);
#ifdef JEMALLOC_TRACE
static arena_t	*trace_arena(const void *ptr);
static void	trace_flush(arena_t *arena);
static void	trace_write(arena_t *arena, const char *s);
static void	trace_thread_cleanup(void *arg);
static void	malloc_trace_flush_all(void);
static void	trace_malloc(const void *ptr, size_t size);
static void	trace_calloc(const void *ptr, size_t number, size_t size);
static void	trace_posix_memalign(const void *ptr, size_t alignment,
    size_t size);
static void	trace_realloc(const void *ptr, const void *old_ptr,
    size_t size, size_t old_size);
static void	trace_free(const void *ptr, size_t size);
static void	trace_malloc_usable_size(size_t size, const void *ptr);
static void	trace_thread_exit(void);
static unsigned	trace_get_tid(void);
#endif
#ifdef JEMALLOC_DEBUG
static void	small_size2bin_validate(void);
#endif
static bool	small_size2bin_init(void);
static bool	small_size2bin_init_hard(void);
static unsigned	malloc_ncpus(void);
static bool	malloc_init_hard(void);
static void	jemalloc_prefork(void);
static void	jemalloc_postfork(void);

/*
 * End function prototypes.
 */
/******************************************************************************/

static void
wrtmessage(const char *p1, const char *p2, const char *p3, const char *p4)
{

	if (write(STDERR_FILENO, p1, strlen(p1)) < 0
	    || write(STDERR_FILENO, p2, strlen(p2)) < 0
	    || write(STDERR_FILENO, p3, strlen(p3)) < 0
	    || write(STDERR_FILENO, p4, strlen(p4)) < 0)
		return;
}

void	(*malloc_message)(const char *p1, const char *p2, const char *p3,
	    const char *p4) = wrtmessage;

/*
 * We don't want to depend on vsnprintf() for production builds, since that can
 * cause unnecessary bloat for static binaries.  umax2s() provides minimal
 * integer printing functionality, so that malloc_printf() use can be limited to
 * JEMALLOC_STATS code.
 */
#define	UMAX2S_BUFSIZE	65
static char *
umax2s(uintmax_t x, unsigned base, char *s)
{
	unsigned i;

	i = UMAX2S_BUFSIZE - 1;
	s[i] = '\0';
	switch (base) {
	case 10:
		do {
			i--;
			s[i] = "0123456789"[x % 10];
			x /= 10;
		} while (x > 0);
		break;
	case 16:
		do {
			i--;
			s[i] = "0123456789abcdef"[x & 0xf];
			x >>= 4;
		} while (x > 0);
		break;
	default:
		do {
			i--;
			s[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[x % base];
			x /= base;
		} while (x > 0);
	}

	return (&s[i]);
}

/*
 * Define a custom assert() in order to reduce the chances of deadlock during
 * assertion failure.
 */
#ifdef JEMALLOC_DEBUG
#  define assert(e) do {						\
	if (!(e)) {							\
		char line_buf[UMAX2S_BUFSIZE];				\
		malloc_message("<jemalloc>: ", __FILE__, ":",		\
		    umax2s(__LINE__, 10, line_buf));			\
		malloc_message(": Failed assertion: ", "\"", #e,	\
		    "\"\n");						\
		abort();						\
	}								\
} while (0)
#else
#define assert(e)
#endif

#ifdef JEMALLOC_STATS
/*
 * Print to stderr in such a way as to (hopefully) avoid memory allocation.
 */
static void
malloc_printf(const char *format, ...)
{
	char buf[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	malloc_message(buf, "", "", "");
}
#endif

/******************************************************************************/
/*
 * Begin pthreads integration.  We intercept pthread_create() calls in order
 * to toggle isthreaded if the process goes multi-threaded.
 */

#ifdef JEMALLOC_LAZY_LOCK
int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_once(void)
{

	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
	if (pthread_create_fptr == NULL) {
		malloc_message("<jemalloc>",
		    ": Error in dlsym(RTLD_NEXT, \"pthread_create\")\n", "",
		    "");
		abort();
	}

	isthreaded = true;
}

int
pthread_create(pthread_t *__restrict thread,
    const pthread_attr_t *__restrict attr, void *(*start_routine)(void *),
    void *__restrict arg)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	pthread_once(&once_control, pthread_create_once);

	return (pthread_create_fptr(thread, attr, start_routine, arg));
}
#endif

/******************************************************************************/
/*
 * Begin mutex.
 */

static bool
malloc_mutex_init(malloc_mutex_t *mutex)
{
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0)
		return (true);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
	if (pthread_mutex_init(mutex, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return (true);
	}
	pthread_mutexattr_destroy(&attr);

	return (false);
}

static inline void
malloc_mutex_lock(malloc_mutex_t *mutex)
{

	if (isthreaded)
		pthread_mutex_lock(mutex);
}

static inline void
malloc_mutex_unlock(malloc_mutex_t *mutex)
{

	if (isthreaded)
		pthread_mutex_unlock(mutex);
}

/*
 * End mutex.
 */
/******************************************************************************/
/*
 * Begin Utility functions/macros.
 */

/* Return the chunk address for allocation address a. */
#define	CHUNK_ADDR2BASE(a)						\
	((void *)((uintptr_t)(a) & ~chunksize_mask))

/* Return the chunk offset of address a. */
#define	CHUNK_ADDR2OFFSET(a)						\
	((size_t)((uintptr_t)(a) & chunksize_mask))

/* Return the smallest chunk multiple that is >= s. */
#define	CHUNK_CEILING(s)						\
	(((s) + chunksize_mask) & ~chunksize_mask)

/* Return the smallest quantum multiple that is >= a. */
#define	QUANTUM_CEILING(a)						\
	(((a) + QUANTUM_MASK) & ~QUANTUM_MASK)

/* Return the smallest cacheline multiple that is >= s. */
#define	CACHELINE_CEILING(s)						\
	(((s) + CACHELINE_MASK) & ~CACHELINE_MASK)

/* Return the smallest subpage multiple that is >= s. */
#define	SUBPAGE_CEILING(s)						\
	(((s) + SUBPAGE_MASK) & ~SUBPAGE_MASK)

/* Return the smallest medium size class that is >= s. */
#define	MEDIUM_CEILING(s)						\
	(((s) + mspace_mask) & ~mspace_mask)

/* Return the smallest pagesize multiple that is >= s. */
#define	PAGE_CEILING(s)							\
	(((s) + PAGE_MASK) & ~PAGE_MASK)

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

/******************************************************************************/

#ifdef JEMALLOC_DSS
static bool
base_pages_alloc_dss(size_t minsize)
{

	/*
	 * Do special DSS allocation here, since base allocations don't need to
	 * be chunk-aligned.
	 */
	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		intptr_t incr;
		size_t csize = CHUNK_CEILING(minsize);

		do {
			/* Get the current end of the DSS. */
			dss_max = sbrk(0);

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.  Don't worry about
			 * dss_max not being chunk-aligned though.
			 */
			incr = (intptr_t)chunksize
			    - (intptr_t)CHUNK_ADDR2OFFSET(dss_max);
			assert(incr >= 0);
			if ((size_t)incr < minsize)
				incr += csize;

			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = (void *)((intptr_t)dss_prev + incr);
				base_pages = dss_prev;
				base_next_addr = base_pages;
				base_past_addr = dss_max;
#ifdef JEMALLOC_STATS
				base_mapped += incr;
#endif
				malloc_mutex_unlock(&dss_mtx);
				return (false);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (true);
}
#endif

static bool
base_pages_alloc_mmap(size_t minsize)
{
	size_t csize;

	assert(minsize != 0);
	csize = PAGE_CEILING(minsize);
	base_pages = pages_map(NULL, csize);
	if (base_pages == NULL)
		return (true);
	base_next_addr = base_pages;
	base_past_addr = (void *)((uintptr_t)base_pages + csize);
#ifdef JEMALLOC_STATS
	base_mapped += csize;
#endif

	return (false);
}

static bool
base_pages_alloc(size_t minsize)
{

#ifdef JEMALLOC_DSS
	if (base_pages_alloc_dss(minsize) == false)
		return (false);

	if (minsize != 0)
#endif
	{
		if (base_pages_alloc_mmap(minsize) == false)
			return (false);
	}

	return (true);
}

static void *
base_alloc(size_t size)
{
	void *ret;
	size_t csize;

	/* Round size up to nearest multiple of the cacheline size. */
	csize = CACHELINE_CEILING(size);

	malloc_mutex_lock(&base_mtx);
	/* Make sure there's enough space for the allocation. */
	if ((uintptr_t)base_next_addr + csize > (uintptr_t)base_past_addr) {
		if (base_pages_alloc(csize)) {
			malloc_mutex_unlock(&base_mtx);
			return (NULL);
		}
	}
	/* Allocate. */
	ret = base_next_addr;
	base_next_addr = (void *)((uintptr_t)base_next_addr + csize);
	malloc_mutex_unlock(&base_mtx);

	return (ret);
}

static extent_node_t *
base_node_alloc(void)
{
	extent_node_t *ret;

	malloc_mutex_lock(&base_mtx);
	if (base_nodes != NULL) {
		ret = base_nodes;
		base_nodes = *(extent_node_t **)ret;
		malloc_mutex_unlock(&base_mtx);
	} else {
		malloc_mutex_unlock(&base_mtx);
		ret = (extent_node_t *)base_alloc(sizeof(extent_node_t));
	}

	return (ret);
}

static void
base_node_dealloc(extent_node_t *node)
{

	malloc_mutex_lock(&base_mtx);
	*(extent_node_t **)node = base_nodes;
	base_nodes = node;
	malloc_mutex_unlock(&base_mtx);
}

/*
 * End Utility functions/macros.
 */
/******************************************************************************/
/*
 * Begin extent tree code.
 */

#ifdef JEMALLOC_DSS
static inline int
extent_szad_comp(extent_node_t *a, extent_node_t *b)
{
	int ret;
	size_t a_size = a->size;
	size_t b_size = b->size;

	ret = (a_size > b_size) - (a_size < b_size);
	if (ret == 0) {
		uintptr_t a_addr = (uintptr_t)a->addr;
		uintptr_t b_addr = (uintptr_t)b->addr;

		ret = (a_addr > b_addr) - (a_addr < b_addr);
	}

	return (ret);
}

/* Wrap red-black tree macros in functions. */
rb_wrap(static JEMALLOC_ATTR(unused), extent_tree_szad_, extent_tree_t,
    extent_node_t, link_szad, extent_szad_comp)
#endif

static inline int
extent_ad_comp(extent_node_t *a, extent_node_t *b)
{
	uintptr_t a_addr = (uintptr_t)a->addr;
	uintptr_t b_addr = (uintptr_t)b->addr;

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

/* Wrap red-black tree macros in functions. */
rb_wrap(static JEMALLOC_ATTR(unused), extent_tree_ad_, extent_tree_t,
    extent_node_t, link_ad, extent_ad_comp)

/*
 * End extent tree code.
 */
/******************************************************************************/
/*
 * Begin chunk management functions.
 */

static void *
pages_map(void *addr, size_t size)
{
	void *ret;

	/*
	 * We don't use MAP_FIXED here, because it can cause the *replacement*
	 * of existing mappings, and we only want to create new mappings.
	 */
	ret = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
	    -1, 0);
	assert(ret != NULL);

	if (ret == MAP_FAILED)
		ret = NULL;
	else if (addr != NULL && ret != addr) {
		/*
		 * We succeeded in mapping memory, but not in the right place.
		 */
		if (munmap(ret, size) == -1) {
			char buf[STRERROR_BUF];

			strerror_r(errno, buf, sizeof(buf));
			malloc_message("<jemalloc>",
			    ": Error in munmap(): ", buf, "\n");
			if (opt_abort)
				abort();
		}
		ret = NULL;
	}

	assert(ret == NULL || (addr == NULL && ret != addr)
	    || (addr != NULL && ret == addr));
	return (ret);
}

static void
pages_unmap(void *addr, size_t size)
{

	if (munmap(addr, size) == -1) {
		char buf[STRERROR_BUF];

		strerror_r(errno, buf, sizeof(buf));
		malloc_message("<jemalloc>",
		    ": Error in munmap(): ", buf, "\n");
		if (opt_abort)
			abort();
	}
}

#ifdef JEMALLOC_DSS
static void *
chunk_alloc_dss(size_t size)
{

	/*
	 * sbrk() uses a signed increment argument, so take care not to
	 * interpret a huge allocation request as a negative increment.
	 */
	if ((intptr_t)size < 0)
		return (NULL);

	malloc_mutex_lock(&dss_mtx);
	if (dss_prev != (void *)-1) {
		intptr_t incr;

		/*
		 * The loop is necessary to recover from races with other
		 * threads that are using the DSS for something other than
		 * malloc.
		 */
		do {
			void *ret;

			/* Get the current end of the DSS. */
			dss_max = sbrk(0);

			/*
			 * Calculate how much padding is necessary to
			 * chunk-align the end of the DSS.
			 */
			incr = (intptr_t)size
			    - (intptr_t)CHUNK_ADDR2OFFSET(dss_max);
			if (incr == (intptr_t)size)
				ret = dss_max;
			else {
				ret = (void *)((intptr_t)dss_max + incr);
				incr += size;
			}

			dss_prev = sbrk(incr);
			if (dss_prev == dss_max) {
				/* Success. */
				dss_max = (void *)((intptr_t)dss_prev + incr);
				malloc_mutex_unlock(&dss_mtx);
				return (ret);
			}
		} while (dss_prev != (void *)-1);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}

static void *
chunk_recycle_dss(size_t size, bool zero)
{
	extent_node_t *node, key;

	key.addr = NULL;
	key.size = size;
	malloc_mutex_lock(&dss_mtx);
	node = extent_tree_szad_nsearch(&dss_chunks_szad, &key);
	if (node != NULL) {
		void *ret = node->addr;

		/* Remove node from the tree. */
		extent_tree_szad_remove(&dss_chunks_szad, node);
		if (node->size == size) {
			extent_tree_ad_remove(&dss_chunks_ad, node);
			base_node_dealloc(node);
		} else {
			/*
			 * Insert the remainder of node's address range as a
			 * smaller chunk.  Its position within dss_chunks_ad
			 * does not change.
			 */
			assert(node->size > size);
			node->addr = (void *)((uintptr_t)node->addr + size);
			node->size -= size;
			extent_tree_szad_insert(&dss_chunks_szad, node);
		}
		malloc_mutex_unlock(&dss_mtx);

		if (zero)
			memset(ret, 0, size);
		return (ret);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (NULL);
}
#endif

static void *
chunk_alloc_mmap_slow(size_t size, bool unaligned)
{
	void *ret;
	size_t offset;

	/* Beware size_t wrap-around. */
	if (size + chunksize <= size)
		return (NULL);

	ret = pages_map(NULL, size + chunksize);
	if (ret == NULL)
		return (NULL);

	/* Clean up unneeded leading/trailing space. */
	offset = CHUNK_ADDR2OFFSET(ret);
	if (offset != 0) {
		/* Note that mmap() returned an unaligned mapping. */
		unaligned = true;

		/* Leading space. */
		pages_unmap(ret, chunksize - offset);

		ret = (void *)((uintptr_t)ret +
		    (chunksize - offset));

		/* Trailing space. */
		pages_unmap((void *)((uintptr_t)ret + size),
		    offset);
	} else {
		/* Trailing space only. */
		pages_unmap((void *)((uintptr_t)ret + size),
		    chunksize);
	}

	/*
	 * If mmap() returned an aligned mapping, reset mmap_unaligned so that
	 * the next chunk_alloc_mmap() execution tries the fast allocation
	 * method.
	 */
	if (unaligned == false)
		mmap_unaligned = false;

	return (ret);
}

static void *
chunk_alloc_mmap(size_t size)
{
	void *ret;

	/*
	 * Ideally, there would be a way to specify alignment to mmap() (like
	 * NetBSD has), but in the absence of such a feature, we have to work
	 * hard to efficiently create aligned mappings.  The reliable, but
	 * slow method is to create a mapping that is over-sized, then trim the
	 * excess.  However, that always results in at least one call to
	 * pages_unmap().
	 *
	 * A more optimistic approach is to try mapping precisely the right
	 * amount, then try to append another mapping if alignment is off.  In
	 * practice, this works out well as long as the application is not
	 * interleaving mappings via direct mmap() calls.  If we do run into a
	 * situation where there is an interleaved mapping and we are unable to
	 * extend an unaligned mapping, our best option is to switch to the
	 * slow method until mmap() returns another aligned mapping.  This will
	 * tend to leave a gap in the memory map that is too small to cause
	 * later problems for the optimistic method.
	 *
	 * Another possible confounding factor is address space layout
	 * randomization (ASLR), which causes mmap(2) to disregard the
	 * requested address.  mmap_unaligned tracks whether the previous
	 * chunk_alloc_mmap() execution received any unaligned or relocated
	 * mappings, and if so, the current execution will immediately fall
	 * back to the slow method.  However, we keep track of whether the fast
	 * method would have succeeded, and if so, we make a note to try the
	 * fast method next time.
	 */

	if (mmap_unaligned == false) {
		size_t offset;

		ret = pages_map(NULL, size);
		if (ret == NULL)
			return (NULL);

		offset = CHUNK_ADDR2OFFSET(ret);
		if (offset != 0) {
			mmap_unaligned = true;
			/* Try to extend chunk boundary. */
			if (pages_map((void *)((uintptr_t)ret + size),
			    chunksize - offset) == NULL) {
				/*
				 * Extension failed.  Clean up, then revert to
				 * the reliable-but-expensive method.
				 */
				pages_unmap(ret, size);
				ret = chunk_alloc_mmap_slow(size, true);
			} else {
				/* Clean up unneeded leading space. */
				pages_unmap(ret, chunksize - offset);
				ret = (void *)((uintptr_t)ret + (chunksize -
				    offset));
			}
		}
	}
		ret = chunk_alloc_mmap_slow(size, false);

	return (ret);
}

static void *
chunk_alloc(size_t size, bool zero)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_DSS
	ret = chunk_recycle_dss(size, zero);
	if (ret != NULL) {
		goto RETURN;
	}

	ret = chunk_alloc_dss(size);
	if (ret != NULL)
		goto RETURN;

#endif
	ret = chunk_alloc_mmap(size);
	if (ret != NULL)
		goto RETURN;

	/* All strategies for allocation failed. */
	ret = NULL;
RETURN:
#ifdef JEMALLOC_STATS
	if (ret != NULL) {
		stats_chunks.nchunks += (size / chunksize);
		stats_chunks.curchunks += (size / chunksize);
	}
	if (stats_chunks.curchunks > stats_chunks.highchunks)
		stats_chunks.highchunks = stats_chunks.curchunks;
#endif

	assert(CHUNK_ADDR2BASE(ret) == ret);
	return (ret);
}

#ifdef JEMALLOC_DSS
static extent_node_t *
chunk_dealloc_dss_record(void *chunk, size_t size)
{
	extent_node_t *node, *prev, key;

	key.addr = (void *)((uintptr_t)chunk + size);
	node = extent_tree_ad_nsearch(&dss_chunks_ad, &key);
	/* Try to coalesce forward. */
	if (node != NULL && node->addr == key.addr) {
		/*
		 * Coalesce chunk with the following address range.  This does
		 * not change the position within dss_chunks_ad, so only
		 * remove/insert from/into dss_chunks_szad.
		 */
		extent_tree_szad_remove(&dss_chunks_szad, node);
		node->addr = chunk;
		node->size += size;
		extent_tree_szad_insert(&dss_chunks_szad, node);
	} else {
		/*
		 * Coalescing forward failed, so insert a new node.  Drop
		 * dss_mtx during node allocation, since it is possible that a
		 * new base chunk will be allocated.
		 */
		malloc_mutex_unlock(&dss_mtx);
		node = base_node_alloc();
		malloc_mutex_lock(&dss_mtx);
		if (node == NULL)
			return (NULL);
		node->addr = chunk;
		node->size = size;
		extent_tree_ad_insert(&dss_chunks_ad, node);
		extent_tree_szad_insert(&dss_chunks_szad, node);
	}

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(&dss_chunks_ad, node);
	if (prev != NULL && (void *)((uintptr_t)prev->addr + prev->size) ==
	    chunk) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within dss_chunks_ad, so only
		 * remove/insert node from/into dss_chunks_szad.
		 */
		extent_tree_szad_remove(&dss_chunks_szad, prev);
		extent_tree_ad_remove(&dss_chunks_ad, prev);

		extent_tree_szad_remove(&dss_chunks_szad, node);
		node->addr = prev->addr;
		node->size += prev->size;
		extent_tree_szad_insert(&dss_chunks_szad, node);

		base_node_dealloc(prev);
	}

	return (node);
}

static bool
chunk_dealloc_dss(void *chunk, size_t size)
{

	malloc_mutex_lock(&dss_mtx);
	if ((uintptr_t)chunk >= (uintptr_t)dss_base
	    && (uintptr_t)chunk < (uintptr_t)dss_max) {
		extent_node_t *node;

		/* Try to coalesce with other unused chunks. */
		node = chunk_dealloc_dss_record(chunk, size);
		if (node != NULL) {
			chunk = node->addr;
			size = node->size;
		}

		/* Get the current end of the DSS. */
		dss_max = sbrk(0);

		/*
		 * Try to shrink the DSS if this chunk is at the end of the
		 * DSS.  The sbrk() call here is subject to a race condition
		 * with threads that use brk(2) or sbrk(2) directly, but the
		 * alternative would be to leak memory for the sake of poorly
		 * designed multi-threaded programs.
		 */
		if ((void *)((uintptr_t)chunk + size) == dss_max
		    && (dss_prev = sbrk(-(intptr_t)size)) == dss_max) {
			/* Success. */
			dss_max = (void *)((intptr_t)dss_prev - (intptr_t)size);

			if (node != NULL) {
				extent_tree_szad_remove(&dss_chunks_szad, node);
				extent_tree_ad_remove(&dss_chunks_ad, node);
				base_node_dealloc(node);
			}
			malloc_mutex_unlock(&dss_mtx);
		} else {
			malloc_mutex_unlock(&dss_mtx);
			madvise(chunk, size, MADV_DONTNEED);
		}

		return (false);
	}
	malloc_mutex_unlock(&dss_mtx);

	return (true);
}
#endif

static void
chunk_dealloc_mmap(void *chunk, size_t size)
{

	pages_unmap(chunk, size);
}

static void
chunk_dealloc(void *chunk, size_t size)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

#ifdef JEMALLOC_STATS
	stats_chunks.curchunks -= (size / chunksize);
#endif

#ifdef JEMALLOC_DSS
	if (chunk_dealloc_dss(chunk, size) == false)
		return;

#endif
	chunk_dealloc_mmap(chunk, size);
}

/*
 * End chunk management functions.
 */
/******************************************************************************/
/*
 * Begin arena.
 */

/*
 * Choose an arena based on a per-thread value (fast-path code, calls slow-path
 * code if necessary).
 */
static inline arena_t *
choose_arena(void)
{
	arena_t *ret;

	/*
	 * We can only use TLS if this is a PIC library, since for the static
	 * library version, libc's malloc is used by TLS allocation, which
	 * introduces a bootstrapping issue.
	 */
#ifndef NO_TLS
	ret = arenas_map;
	if (ret == NULL) {
		ret = choose_arena_hard();
		assert(ret != NULL);
	}
#else
	if (isthreaded && narenas > 1) {
		unsigned long ind;

		/*
		 * Hash pthread_self() to one of the arenas.  There is a prime
		 * number of arenas, so this has a reasonable chance of
		 * working.  Even so, the hashing can be easily thwarted by
		 * inconvenient pthread_self() values.  Without specific
		 * knowledge of how pthread_self() calculates values, we can't
		 * easily do much better than this.
		 */
		ind = (unsigned long) pthread_self() % narenas;

		/*
		 * Optimistially assume that arenas[ind] has been initialized.
		 * At worst, we find out that some other thread has already
		 * done so, after acquiring the lock in preparation.  Note that
		 * this lazy locking also has the effect of lazily forcing
		 * cache coherency; without the lock acquisition, there's no
		 * guarantee that modification of arenas[ind] by another thread
		 * would be seen on this CPU for an arbitrary amount of time.
		 *
		 * In general, this approach to modifying a synchronized value
		 * isn't a good idea, but in this case we only ever modify the
		 * value once, so things work out well.
		 */
		ret = arenas[ind];
		if (ret == NULL) {
			/*
			 * Avoid races with another thread that may have already
			 * initialized arenas[ind].
			 */
			malloc_mutex_lock(&arenas_lock);
			if (arenas[ind] == NULL)
				ret = arenas_extend((unsigned)ind);
			else
				ret = arenas[ind];
			malloc_mutex_unlock(&arenas_lock);
		}
	} else
		ret = arenas[0];
#endif

	assert(ret != NULL);
	return (ret);
}

#ifndef NO_TLS
/*
 * Choose an arena based on a per-thread value (slow-path code only, called
 * only by choose_arena()).
 */
static arena_t *
choose_arena_hard(void)
{
	arena_t *ret;

	assert(isthreaded);

	if (narenas > 1) {
		malloc_mutex_lock(&arenas_lock);
		if ((ret = arenas[next_arena]) == NULL)
			ret = arenas_extend(next_arena);
		next_arena = (next_arena + 1) % narenas;
		malloc_mutex_unlock(&arenas_lock);
	} else
		ret = arenas[0];

	arenas_map = ret;

	return (ret);
}
#endif

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

#ifdef JEMALLOC_TCACHE
static inline void
tcache_event(tcache_t *tcache)
{

	if (tcache_gc_incr == 0)
		return;

	tcache->ev_cnt++;
	assert(tcache->ev_cnt <= tcache_gc_incr);
	if (tcache->ev_cnt >= tcache_gc_incr) {
		size_t binind = tcache->next_gc_bin;
		tcache_bin_t *tbin = tcache->tbins[binind];

		if (tbin != NULL) {
			if (tbin->high_water == 0) {
				/*
				 * This bin went completely unused for an
				 * entire GC cycle, so throw away the tbin.
				 */
				assert(tbin->ncached == 0);
				tcache_bin_destroy(tcache, tbin, binind);
				tcache->tbins[binind] = NULL;
			} else {
				if (tbin->low_water > 0) {
					/*
					 * Flush (ceiling) half of the objects
					 * below the low water mark.
					 */
					tcache_bin_flush(tbin, binind,
					    tbin->ncached - (tbin->low_water >>
					    1) - (tbin->low_water & 1));
				}
				tbin->low_water = tbin->ncached;
				tbin->high_water = tbin->ncached;
			}
		}

		tcache->next_gc_bin++;
		if (tcache->next_gc_bin == nbins)
			tcache->next_gc_bin = 0;
		tcache->ev_cnt = 0;
	}
}

static inline void *
tcache_bin_alloc(tcache_bin_t *tbin)
{

	if (tbin->ncached == 0)
		return (NULL);
	tbin->ncached--;
	if (tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
	return (tbin->slots[tbin->ncached]);
}

static void
tcache_bin_fill(tcache_t *tcache, tcache_bin_t *tbin, size_t binind)
{
	arena_t *arena;
	arena_bin_t *bin;
	arena_run_t *run;
	void *ptr;
	unsigned i;

	assert(tbin->ncached == 0);

	arena = tcache->arena;
	bin = &arena->bins[binind];
	malloc_mutex_lock(&arena->lock);
	for (i = 0; i < (TCACHE_NSLOTS >> 1); i++) {
		if ((run = bin->runcur) != NULL && run->nfree > 0)
			ptr = arena_bin_malloc_easy(arena, bin, run);
		else
			ptr = arena_bin_malloc_hard(arena, bin);
		if (ptr == NULL)
			break;
		/*
		 * Fill tbin such that the objects lowest in memory are used
		 * first.
		 */
		tbin->slots[(TCACHE_NSLOTS >> 1) - 1 - i] = ptr;
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

static inline void *
tcache_alloc(tcache_t *tcache, size_t size, bool zero)
{
	void *ret;
	tcache_bin_t *tbin;
	size_t binind;

	if (size <= small_maxclass)
		binind = small_size2bin[size];
	else {
		binind = mbin0 + ((MEDIUM_CEILING(size) - medium_min) >>
		    lg_mspace);
	}
	assert(binind < nbins);
	tbin = tcache->tbins[binind];
	if (tbin == NULL) {
		tbin = tcache_bin_create(tcache->arena);
		if (tbin == NULL)
			return (NULL);
		tcache->tbins[binind] = tbin;
	}

	ret = tcache_bin_alloc(tbin);
	if (ret == NULL) {
		ret = tcache_alloc_hard(tcache, tbin, binind);
		if (ret == NULL)
			return (NULL);
	}

	if (zero == false) {
#ifdef JEMALLOC_FILL
		if (opt_junk)
			memset(ret, 0xa5, size);
		else if (opt_zero)
			memset(ret, 0, size);
#endif
	} else
		memset(ret, 0, size);

#ifdef JEMALLOC_STATS
	tbin->tstats.nrequests++;
#endif
	tcache_event(tcache);
	return (ret);
}

static void *
tcache_alloc_hard(tcache_t *tcache, tcache_bin_t *tbin, size_t binind)
{
	void *ret;

	tcache_bin_fill(tcache, tbin, binind);
	ret = tcache_bin_alloc(tbin);

	return (ret);
}
#endif

static inline void *
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

static void *
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
	arena->stats.lstats[size >> PAGE_SHIFT].nrequests++;
	arena->stats.lstats[size >> PAGE_SHIFT].curruns++;
	if (arena->stats.lstats[size >> PAGE_SHIFT].curruns >
	    arena->stats.lstats[size >> PAGE_SHIFT].highruns) {
		arena->stats.lstats[size >> PAGE_SHIFT].highruns =
		    arena->stats.lstats[size >> PAGE_SHIFT].curruns;
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

static inline void *
arena_malloc(size_t size, bool zero)
{

	assert(size != 0);
	assert(QUANTUM_CEILING(size) <= arena_maxclass);

	if (size <= bin_maxclass) {
#ifdef JEMALLOC_TCACHE
		if (isthreaded && opt_tcache) {
			tcache_t *tcache = tcache_tls;
			if (tcache == NULL) {
				tcache = tcache_create(choose_arena());
				if (tcache == NULL)
					return (NULL);
			}
			return (tcache_alloc(tcache, size, zero));
		} else {
#endif
			if (size <= small_maxclass) {
				return (arena_malloc_small(choose_arena(), size,
				    zero));
			} else {
				return (arena_malloc_medium(choose_arena(),
				    size, zero));
			}
#ifdef JEMALLOC_TCACHE
		}
#endif
	} else
		return (arena_malloc_large(choose_arena(), size, zero));
}

static inline void *
imalloc(size_t size)
{

	assert(size != 0);

	if (size <= arena_maxclass)
		return (arena_malloc(size, false));
	else
		return (huge_malloc(size, false));
}

static inline void *
icalloc(size_t size)
{

	if (size <= arena_maxclass)
		return (arena_malloc(size, true));
	else
		return (huge_malloc(size, true));
}

/* Only handles large allocations that require more than page alignment. */
static void *
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
	arena->stats.lstats[size >> PAGE_SHIFT].nrequests++;
	arena->stats.lstats[size >> PAGE_SHIFT].curruns++;
	if (arena->stats.lstats[size >> PAGE_SHIFT].curruns >
	    arena->stats.lstats[size >> PAGE_SHIFT].highruns) {
		arena->stats.lstats[size >> PAGE_SHIFT].highruns =
		    arena->stats.lstats[size >> PAGE_SHIFT].curruns;
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

static inline void *
ipalloc(size_t alignment, size_t size)
{
	void *ret;
	size_t ceil_size;

	/*
	 * Round size up to the nearest multiple of alignment.
	 *
	 * This done, we can take advantage of the fact that for each small
	 * size class, every object is aligned at the smallest power of two
	 * that is non-zero in the base two representation of the size.  For
	 * example:
	 *
	 *   Size |   Base 2 | Minimum alignment
	 *   -----+----------+------------------
	 *     96 |  1100000 |  32
	 *    144 | 10100000 |  32
	 *    192 | 11000000 |  64
	 *
	 * Depending on runtime settings, it is possible that arena_malloc()
	 * will further round up to a power of two, but that never causes
	 * correctness issues.
	 */
	ceil_size = (size + (alignment - 1)) & (-alignment);
	/*
	 * (ceil_size < size) protects against the combination of maximal
	 * alignment and size greater than maximal alignment.
	 */
	if (ceil_size < size) {
		/* size_t overflow. */
		return (NULL);
	}

	if (ceil_size <= PAGE_SIZE || (alignment <= PAGE_SIZE
	    && ceil_size <= arena_maxclass))
		ret = arena_malloc(ceil_size, false);
	else {
		size_t run_size;

		/*
		 * We can't achieve subpage alignment, so round up alignment
		 * permanently; it makes later calculations simpler.
		 */
		alignment = PAGE_CEILING(alignment);
		ceil_size = PAGE_CEILING(size);
		/*
		 * (ceil_size < size) protects against very large sizes within
		 * PAGE_SIZE of SIZE_T_MAX.
		 *
		 * (ceil_size + alignment < ceil_size) protects against the
		 * combination of maximal alignment and ceil_size large enough
		 * to cause overflow.  This is similar to the first overflow
		 * check above, but it needs to be repeated due to the new
		 * ceil_size value, which may now be *equal* to maximal
		 * alignment, whereas before we only detected overflow if the
		 * original size was *greater* than maximal alignment.
		 */
		if (ceil_size < size || ceil_size + alignment < ceil_size) {
			/* size_t overflow. */
			return (NULL);
		}

		/*
		 * Calculate the size of the over-size run that arena_palloc()
		 * would need to allocate in order to guarantee the alignment.
		 */
		if (ceil_size >= alignment)
			run_size = ceil_size + alignment - PAGE_SIZE;
		else {
			/*
			 * It is possible that (alignment << 1) will cause
			 * overflow, but it doesn't matter because we also
			 * subtract PAGE_SIZE, which in the case of overflow
			 * leaves us with a very large run_size.  That causes
			 * the first conditional below to fail, which means
			 * that the bogus run_size value never gets used for
			 * anything important.
			 */
			run_size = (alignment << 1) - PAGE_SIZE;
		}

		if (run_size <= arena_maxclass) {
			ret = arena_palloc(choose_arena(), alignment, ceil_size,
			    run_size);
		} else if (alignment <= chunksize)
			ret = huge_malloc(ceil_size, false);
		else
			ret = huge_palloc(alignment, ceil_size);
	}

	assert(((uintptr_t)ret & (alignment - 1)) == 0);
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
static size_t
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

static inline size_t
isalloc(const void *ptr)
{
	size_t ret;
	arena_chunk_t *chunk;

	assert(ptr != NULL);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if (chunk != ptr) {
		/* Region. */
		assert(chunk->arena->magic == ARENA_MAGIC);

		ret = arena_salloc(ptr);
	} else {
		extent_node_t *node, key;

		/* Chunk (huge allocation). */

		malloc_mutex_lock(&huge_mtx);

		/* Extract from tree of huge allocations. */
		key.addr = __DECONST(void *, ptr);
		node = extent_tree_ad_search(&huge, &key);
		assert(node != NULL);

		ret = node->size;

		malloc_mutex_unlock(&huge_mtx);
	}

	return (ret);
}

static inline void
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

#ifdef JEMALLOC_STATS
static void
arena_stats_print(arena_t *arena, bool bins, bool large)
{

	malloc_printf("dirty pages: %zu:%zu active:dirty, %llu sweep%s,"
	    " %llu madvise%s, %llu purged\n",
	    arena->nactive, arena->ndirty,
	    arena->stats.npurge, arena->stats.npurge == 1 ? "" : "s",
	    arena->stats.nmadvise, arena->stats.nmadvise == 1 ? "" : "s",
	    arena->stats.purged);

	malloc_printf("            allocated      nmalloc      ndalloc\n");
	malloc_printf("small:   %12zu %12llu %12llu\n",
	    arena->stats.allocated_small, arena->stats.nmalloc_small,
	    arena->stats.ndalloc_small);
	malloc_printf("medium:  %12zu %12llu %12llu\n",
	    arena->stats.allocated_medium, arena->stats.nmalloc_medium,
	    arena->stats.ndalloc_medium);
	malloc_printf("large:   %12zu %12llu %12llu\n",
	    arena->stats.allocated_large, arena->stats.nmalloc_large,
	    arena->stats.ndalloc_large);
	malloc_printf("total:   %12zu %12llu %12llu\n",
	    arena->stats.allocated_small + arena->stats.allocated_medium +
	    arena->stats.allocated_large, arena->stats.nmalloc_small +
	    arena->stats.nmalloc_medium + arena->stats.nmalloc_large,
	    arena->stats.ndalloc_small + arena->stats.ndalloc_medium +
	    arena->stats.ndalloc_large);
	malloc_printf("mapped:  %12zu\n", arena->stats.mapped);

	if (bins && arena->stats.nmalloc_small + arena->stats.nmalloc_medium >
	    0) {
		unsigned i, gap_start;
#ifdef JEMALLOC_TCACHE
		malloc_printf("bins:     bin    size regs pgs  requests    "
		    "nfills  nflushes   newruns    reruns maxruns curruns\n");
#else
		malloc_printf("bins:     bin    size regs pgs  requests   "
		    "newruns    reruns maxruns curruns\n");
#endif
		for (i = 0, gap_start = UINT_MAX; i < nbins; i++) {
			if (arena->bins[i].stats.nruns == 0) {
				if (gap_start == UINT_MAX)
					gap_start = i;
			} else {
				if (gap_start != UINT_MAX) {
					if (i > gap_start + 1) {
						/*
						 * Gap of more than one size
						 * class.
						 */
						malloc_printf("[%u..%u]\n",
						    gap_start, i - 1);
					} else {
						/* Gap of one size class. */
						malloc_printf("[%u]\n",
						    gap_start);
					}
					gap_start = UINT_MAX;
				}
				malloc_printf(
				    "%13u %1s %5u %4u %3u %9llu %9llu"
#ifdef JEMALLOC_TCACHE
				    " %9llu %9llu"
#endif
				    " %9llu %7lu %7lu\n",
				    i,
				    i < ntbins ? "T" : i < ntbins + nqbins ?
				    "Q" : i < ntbins + nqbins + ncbins ? "C" :
				    i < ntbins + nqbins + ncbins + nsbins ? "S"
				    : "M",
				    arena->bins[i].reg_size,
				    arena->bins[i].nregs,
				    arena->bins[i].run_size >> PAGE_SHIFT,
				    arena->bins[i].stats.nrequests,
#ifdef JEMALLOC_TCACHE
				    arena->bins[i].stats.nfills,
				    arena->bins[i].stats.nflushes,
#endif
				    arena->bins[i].stats.nruns,
				    arena->bins[i].stats.reruns,
				    arena->bins[i].stats.highruns,
				    arena->bins[i].stats.curruns);
			}
		}
		if (gap_start != UINT_MAX) {
			if (i > gap_start + 1) {
				/* Gap of more than one size class. */
				malloc_printf("[%u..%u]\n", gap_start, i - 1);
			} else {
				/* Gap of one size class. */
				malloc_printf("[%u]\n", gap_start);
			}
		}
	}

	if (large && arena->stats.nmalloc_large > 0) {
		size_t i;
		ssize_t gap_start;
		size_t nlclasses = (chunksize - PAGE_SIZE) >> PAGE_SHIFT;

		malloc_printf(
		    "large:   size pages nrequests   maxruns   curruns\n");

		for (i = 0, gap_start = -1; i < nlclasses; i++) {
			if (arena->stats.lstats[i].nrequests == 0) {
				if (gap_start == -1)
					gap_start = i;
			} else {
				if (gap_start != -1) {
					malloc_printf("[%zu]\n", i - gap_start);
					gap_start = -1;
				}
				malloc_printf("%13zu %5zu %9llu %9zu %9zu\n",
				    (i+1) << PAGE_SHIFT, i+1,
				    arena->stats.lstats[i].nrequests,
				    arena->stats.lstats[i].highruns,
				    arena->stats.lstats[i].curruns);
			}
		}
		if (gap_start != -1)
			malloc_printf("[%zu]\n", i - gap_start);
	}
}
#endif

static void
stats_print_atexit(void)
{

#if (defined(JEMALLOC_TCACHE) && defined(JEMALLOC_STATS))
	unsigned i;

	/*
	 * Merge stats from extant threads.  This is racy, since individual
	 * threads do not lock when recording tcache stats events.  As a
	 * consequence, the final stats may be slightly out of date by the time
	 * they are reported, if other threads continue to allocate.
	 */
	for (i = 0; i < narenas; i++) {
		arena_t *arena = arenas[i];
		if (arena != NULL) {
			tcache_t *tcache;

			malloc_mutex_lock(&arena->lock);
			ql_foreach(tcache, &arena->tcache_ql, link) {
				tcache_stats_merge(tcache, arena);
			}
			malloc_mutex_unlock(&arena->lock);
		}
	}
#endif
	malloc_stats_print(NULL);
}

#ifdef JEMALLOC_TCACHE
static inline void
tcache_bin_merge(void **to, void **fr, unsigned lcnt, unsigned rcnt)
{
	void **l, **r;
	unsigned li, ri, i;

	l = fr;
	r = &fr[lcnt];
	li = ri = i = 0;
	while (li < lcnt && ri < rcnt) {
		/* High pointers come first in sorted result. */
		if ((uintptr_t)l[li] > (uintptr_t)r[ri]) {
			to[i] = l[li];
			li++;
		} else {
			to[i] = r[ri];
			ri++;
		}
		i++;
	}

	if (li < lcnt)
		memcpy(&to[i], &l[li], sizeof(void *) * (lcnt - li));
	else if (ri < rcnt)
		memcpy(&to[i], &r[ri], sizeof(void *) * (rcnt - ri));
}

static inline void
tcache_bin_sort(tcache_bin_t *tbin)
{
	unsigned e, i;
	void **fr, **to;
	void *mslots[TCACHE_NSLOTS];

	/*
	 * Perform iterative merge sort, swapping source and destination arrays
	 * during each iteration.
	 */

	fr = mslots; to = tbin->slots;
	for (e = 1; e < tbin->ncached; e <<= 1) {
		void **tmp = fr; fr = to; to = tmp;
		for (i = 0; i + (e << 1) <= tbin->ncached; i += (e << 1))
			tcache_bin_merge(&to[i], &fr[i], e, e);
		if (i + e <= tbin->ncached) {
			tcache_bin_merge(&to[i], &fr[i],
			    e, tbin->ncached - (i + e));
		} else if (i < tbin->ncached)
			tcache_bin_merge(&to[i], &fr[i], tbin->ncached - i, 0);
	}

	/* Copy the final result out of mslots, if necessary. */
	if (to == mslots)
		memcpy(tbin->slots, mslots, sizeof(void *) * tbin->ncached);

#ifdef JEMALLOC_DEBUG
	for (i = 1; i < tbin->ncached; i++)
		assert(tbin->slots[i-1] > tbin->slots[i]);
#endif
}

static void
tcache_bin_flush(tcache_bin_t *tbin, size_t binind, unsigned rem)
{
	arena_chunk_t *chunk;
	arena_t *arena;
	void *ptr;
	unsigned i, ndeferred, ncached;

	if (rem > 0) {
		assert(rem < tbin->ncached);
		/* Sort pointers such that the highest objects will be freed. */
		tcache_bin_sort(tbin);
	}

	for (ndeferred = tbin->ncached - rem; ndeferred > 0;) {
		ncached = ndeferred;
		/* Lock the arena associated with the first object. */
		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(tbin->slots[0]);
		arena = chunk->arena;
		malloc_mutex_lock(&arena->lock);
		/* Deallocate every object that belongs to the locked arena. */
		for (i = ndeferred = 0; i < ncached; i++) {
			ptr = tbin->slots[i];
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
			if (chunk->arena == arena) {
				size_t pageind = (((uintptr_t)ptr -
				    (uintptr_t)chunk) >> PAGE_SHIFT);
				arena_chunk_map_t *mapelm =
				    &chunk->map[pageind];
				arena_dalloc_bin(arena, chunk, ptr, mapelm);
			} else {
				/*
				 * This object was allocated via a different
				 * arena than the one that is currently locked.
				 * Stash the object, so that it can be handled
				 * in a future pass.
				 */
				tbin->slots[ndeferred] = ptr;
				ndeferred++;
			}
		}
#ifdef JEMALLOC_STATS
		arena->bins[binind].stats.nflushes++;
		{
			arena_bin_t *bin = &arena->bins[binind];
			bin->stats.nrequests += tbin->tstats.nrequests;
			if (bin->reg_size <= small_maxclass) {
				arena->stats.nmalloc_small +=
				    tbin->tstats.nrequests;
			} else {
				arena->stats.nmalloc_medium +=
				    tbin->tstats.nrequests;
			}
			tbin->tstats.nrequests = 0;
		}
#endif
		malloc_mutex_unlock(&arena->lock);
	}

	if (rem > 0) {
		/*
		 * Shift the remaining valid pointers to the base of the slots
		 * array.
		 */
		memmove(&tbin->slots[0], &tbin->slots[tbin->ncached - rem],
		    rem * sizeof(void *));
	}
	tbin->ncached = rem;
}

static inline void
tcache_dalloc(tcache_t *tcache, void *ptr)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	arena_run_t *run;
	arena_bin_t *bin;
	tcache_bin_t *tbin;
	size_t pageind, binind;
	arena_chunk_map_t *mapelm;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    ((mapelm->bits & CHUNK_MAP_PG_MASK) >> CHUNK_MAP_PG_SHIFT)) <<
	    PAGE_SHIFT));
	assert(run->magic == ARENA_RUN_MAGIC);
	bin = run->bin;
	binind = ((uintptr_t)bin - (uintptr_t)&arena->bins) /
	    sizeof(arena_bin_t);
	assert(binind < nbins);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, arena->bins[binind].reg_size);
#endif

	tbin = tcache->tbins[binind];
	if (tbin == NULL) {
		tbin = tcache_bin_create(choose_arena());
		if (tbin == NULL) {
			malloc_mutex_lock(&arena->lock);
			arena_dalloc_bin(arena, chunk, ptr, mapelm);
			malloc_mutex_unlock(&arena->lock);
			return;
		}
		tcache->tbins[binind] = tbin;
	}

	if (tbin->ncached == TCACHE_NSLOTS)
		tcache_bin_flush(tbin, binind, (TCACHE_NSLOTS >> 1));
	assert(tbin->ncached < TCACHE_NSLOTS);
	tbin->slots[tbin->ncached] = ptr;
	tbin->ncached++;
	if (tbin->ncached > tbin->high_water)
		tbin->high_water = tbin->ncached;

	tcache_event(tcache);
}
#endif

static void
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
		arena->stats.lstats[size >> PAGE_SHIFT].curruns--;
#endif
	}
#ifdef JEMALLOC_STATS
	arena->stats.ndalloc_large++;
#endif

	arena_run_dalloc(arena, (arena_run_t *)ptr, true);
	malloc_mutex_unlock(&arena->lock);
}

static inline void
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
		if (isthreaded && opt_tcache) {
			tcache_t *tcache = tcache_tls;
			if ((uintptr_t)tcache > (uintptr_t)1)
				tcache_dalloc(tcache, ptr);
			else {
				arena_dalloc_hard(arena, chunk, ptr, mapelm,
				    tcache);
			}
		} else {
#endif
			malloc_mutex_lock(&arena->lock);
			arena_dalloc_bin(arena, chunk, ptr, mapelm);
			malloc_mutex_unlock(&arena->lock);
#ifdef JEMALLOC_TCACHE
		}
#endif
	} else
		arena_dalloc_large(arena, chunk, ptr);
}

#ifdef JEMALLOC_TCACHE
static void
arena_dalloc_hard(arena_t *arena, arena_chunk_t *chunk, void *ptr,
    arena_chunk_map_t *mapelm, tcache_t *tcache)
{

	if (tcache == NULL) {
		tcache = tcache_create(arena);
		if (tcache == NULL) {
			malloc_mutex_lock(&arena->lock);
			arena_dalloc_bin(arena, chunk, ptr, mapelm);
			malloc_mutex_unlock(&arena->lock);
		} else
			tcache_dalloc(tcache, ptr);
	} else {
		/* This thread is currently exiting, so directly deallocate. */
		assert(tcache == (void *)(uintptr_t)1);
		malloc_mutex_lock(&arena->lock);
		arena_dalloc_bin(arena, chunk, ptr, mapelm);
		malloc_mutex_unlock(&arena->lock);
	}
}
#endif

static inline void
idalloc(void *ptr)
{
	arena_chunk_t *chunk;

	assert(ptr != NULL);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if (chunk != ptr)
		arena_dalloc(chunk->arena, chunk, ptr);
	else
		huge_dalloc(ptr);
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

static void *
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

static inline void *
iralloc(void *ptr, size_t size)
{
	size_t oldsize;

	assert(ptr != NULL);
	assert(size != 0);

	oldsize = isalloc(ptr);

	if (size <= arena_maxclass)
		return (arena_ralloc(ptr, size, oldsize));
	else
		return (huge_ralloc(ptr, size, oldsize));
}

static bool
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
			malloc_message("<jemalloc>",
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

/* Create a new arena and insert it into the arenas array at index ind. */
static arena_t *
arenas_extend(unsigned ind)
{
	arena_t *ret;

	/* Allocate enough space for trailing bins. */
	ret = (arena_t *)base_alloc(sizeof(arena_t)
	    + (sizeof(arena_bin_t) * (nbins - 1)));
	if (ret != NULL && arena_new(ret, ind) == false) {
		arenas[ind] = ret;
		return (ret);
	}
	/* Only reached if there is an OOM error. */

	/*
	 * OOM here is quite inconvenient to propagate, since dealing with it
	 * would require a check for failure in the fast path.  Instead, punt
	 * by using arenas[0].  In practice, this is an extremely unlikely
	 * failure.
	 */
	malloc_message("<jemalloc>",
	    ": Error initializing arena\n", "", "");
	if (opt_abort)
		abort();

	return (arenas[0]);
}

#ifdef JEMALLOC_TCACHE
static tcache_bin_t *
tcache_bin_create(arena_t *arena)
{
	tcache_bin_t *ret;

	if (sizeof(tcache_bin_t) <= small_maxclass) {
		ret = (tcache_bin_t *)arena_malloc_small(arena,
		    sizeof(tcache_bin_t), false);
	} else if (sizeof(tcache_bin_t) <= bin_maxclass) {
		ret = (tcache_bin_t *)arena_malloc_medium(arena,
		    sizeof(tcache_bin_t), false);
	} else
		ret = imalloc(sizeof(tcache_bin_t));
	if (ret == NULL)
		return (NULL);
#ifdef JEMALLOC_STATS
	memset(&ret->tstats, 0, sizeof(tcache_bin_stats_t));
#endif
	ret->low_water = 0;
	ret->high_water = 0;
	ret->ncached = 0;

	return (ret);
}

static void
tcache_bin_destroy(tcache_t *tcache, tcache_bin_t *tbin, unsigned binind)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	size_t pageind;
	arena_chunk_map_t *mapelm;

	chunk = CHUNK_ADDR2BASE(tbin);
	arena = chunk->arena;
	pageind = (((uintptr_t)tbin - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];

#ifdef JEMALLOC_STATS
	if (tbin->tstats.nrequests != 0) {
		arena_t *arena = tcache->arena;
		arena_bin_t *bin = &arena->bins[binind];
		malloc_mutex_lock(&arena->lock);
		bin->stats.nrequests += tbin->tstats.nrequests;
		if (bin->reg_size <= small_maxclass)
			arena->stats.nmalloc_small += tbin->tstats.nrequests;
		else
			arena->stats.nmalloc_medium += tbin->tstats.nrequests;
		malloc_mutex_unlock(&arena->lock);
	}
#endif

	assert(tbin->ncached == 0);
	if (sizeof(tcache_bin_t) <= bin_maxclass) {
		malloc_mutex_lock(&arena->lock);
		arena_dalloc_bin(arena, chunk, tbin, mapelm);
		malloc_mutex_unlock(&arena->lock);
	} else
		idalloc(tbin);
}

#ifdef JEMALLOC_STATS
static void
tcache_stats_merge(tcache_t *tcache, arena_t *arena)
{
	unsigned i;

	/* Merge and reset tcache stats. */
	for (i = 0; i < mbin0; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			bin->stats.nrequests += tbin->tstats.nrequests;
			arena->stats.nmalloc_small += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
	}
	for (; i < nbins; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			bin->stats.nrequests += tbin->tstats.nrequests;
			arena->stats.nmalloc_medium += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
	}
}
#endif

static tcache_t *
tcache_create(arena_t *arena)
{
	tcache_t *tcache;

	if (sizeof(tcache_t) + (sizeof(tcache_bin_t *) * (nbins - 1)) <=
	    small_maxclass) {
		tcache = (tcache_t *)arena_malloc_small(arena, sizeof(tcache_t)
		    + (sizeof(tcache_bin_t *) * (nbins - 1)), true);
	} else if (sizeof(tcache_t) + (sizeof(tcache_bin_t *) * (nbins - 1)) <=
	    bin_maxclass) {
		tcache = (tcache_t *)arena_malloc_medium(arena, sizeof(tcache_t)
		    + (sizeof(tcache_bin_t *) * (nbins - 1)), true);
	} else {
		tcache = (tcache_t *)icalloc(sizeof(tcache_t) +
		    (sizeof(tcache_bin_t *) * (nbins - 1)));
	}

	if (tcache == NULL)
		return (NULL);

#ifdef JEMALLOC_STATS
	/* Link into list of extant tcaches. */
	malloc_mutex_lock(&arena->lock);
	ql_elm_new(tcache, link);
	ql_tail_insert(&arena->tcache_ql, tcache, link);
	malloc_mutex_unlock(&arena->lock);
#endif

	tcache->arena = arena;

	tcache_tls = tcache;
	pthread_setspecific(tcache_tsd, tcache);

	return (tcache);
}

static void
tcache_destroy(tcache_t *tcache)
{
	unsigned i;

#ifdef JEMALLOC_STATS
	/* Unlink from list of extant tcaches. */
	malloc_mutex_lock(&tcache->arena->lock);
	ql_remove(&tcache->arena->tcache_ql, tcache, link);
	tcache_stats_merge(tcache, tcache->arena);
	malloc_mutex_unlock(&tcache->arena->lock);
#endif

	for (i = 0; i < nbins; i++) {
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			tcache_bin_flush(tbin, i, 0);
			tcache_bin_destroy(tcache, tbin, i);
		}
	}

	if (arena_salloc(tcache) <= bin_maxclass) {
		arena_chunk_t *chunk = CHUNK_ADDR2BASE(tcache);
		arena_t *arena = chunk->arena;
		size_t pageind = (((uintptr_t)tcache - (uintptr_t)chunk) >>
		    PAGE_SHIFT);
		arena_chunk_map_t *mapelm = &chunk->map[pageind];

		malloc_mutex_lock(&arena->lock);
		arena_dalloc_bin(arena, chunk, tcache, mapelm);
		malloc_mutex_unlock(&arena->lock);
	} else
		idalloc(tcache);
}

static void
tcache_thread_cleanup(void *arg)
{
	tcache_t *tcache = (tcache_t *)arg;

	assert(tcache == tcache_tls);
	if (tcache != NULL) {
		assert(tcache != (void *)(uintptr_t)1);
		tcache_destroy(tcache);
		tcache_tls = (void *)(uintptr_t)1;
	}
}
#endif

/*
 * End arena.
 */
/******************************************************************************/
/*
 * Begin general internal functions.
 */

static void *
huge_malloc(size_t size, bool zero)
{
	void *ret;
	size_t csize;
	extent_node_t *node;

	/* Allocate one or more contiguous chunks for this request. */

	csize = CHUNK_CEILING(size);
	if (csize == 0) {
		/* size is large enough to cause size_t wrap-around. */
		return (NULL);
	}

	/* Allocate an extent node with which to track the chunk. */
	node = base_node_alloc();
	if (node == NULL)
		return (NULL);

	ret = chunk_alloc(csize, zero);
	if (ret == NULL) {
		base_node_dealloc(node);
		return (NULL);
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = csize;

	malloc_mutex_lock(&huge_mtx);
	extent_tree_ad_insert(&huge, node);
#ifdef JEMALLOC_STATS
	huge_nmalloc++;
	huge_allocated += csize;
#endif
	malloc_mutex_unlock(&huge_mtx);

#ifdef JEMALLOC_FILL
	if (zero == false) {
		if (opt_junk)
			memset(ret, 0xa5, csize);
		else if (opt_zero)
			memset(ret, 0, csize);
	}
#endif

	return (ret);
}

/* Only handles large allocations that require more than chunk alignment. */
static void *
huge_palloc(size_t alignment, size_t size)
{
	void *ret;
	size_t alloc_size, chunk_size, offset;
	extent_node_t *node;

	/*
	 * This allocation requires alignment that is even larger than chunk
	 * alignment.  This means that huge_malloc() isn't good enough.
	 *
	 * Allocate almost twice as many chunks as are demanded by the size or
	 * alignment, in order to assure the alignment can be achieved, then
	 * unmap leading and trailing chunks.
	 */
	assert(alignment >= chunksize);

	chunk_size = CHUNK_CEILING(size);

	if (size >= alignment)
		alloc_size = chunk_size + alignment - chunksize;
	else
		alloc_size = (alignment << 1) - chunksize;

	/* Allocate an extent node with which to track the chunk. */
	node = base_node_alloc();
	if (node == NULL)
		return (NULL);

	ret = chunk_alloc(alloc_size, false);
	if (ret == NULL) {
		base_node_dealloc(node);
		return (NULL);
	}

	offset = (uintptr_t)ret & (alignment - 1);
	assert((offset & chunksize_mask) == 0);
	assert(offset < alloc_size);
	if (offset == 0) {
		/* Trim trailing space. */
		chunk_dealloc((void *)((uintptr_t)ret + chunk_size), alloc_size
		    - chunk_size);
	} else {
		size_t trailsize;

		/* Trim leading space. */
		chunk_dealloc(ret, alignment - offset);

		ret = (void *)((uintptr_t)ret + (alignment - offset));

		trailsize = alloc_size - (alignment - offset) - chunk_size;
		if (trailsize != 0) {
		    /* Trim trailing space. */
		    assert(trailsize < alloc_size);
		    chunk_dealloc((void *)((uintptr_t)ret + chunk_size),
			trailsize);
		}
	}

	/* Insert node into huge. */
	node->addr = ret;
	node->size = chunk_size;

	malloc_mutex_lock(&huge_mtx);
	extent_tree_ad_insert(&huge, node);
#ifdef JEMALLOC_STATS
	huge_nmalloc++;
	huge_allocated += chunk_size;
#endif
	malloc_mutex_unlock(&huge_mtx);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ret, 0xa5, chunk_size);
	else if (opt_zero)
		memset(ret, 0, chunk_size);
#endif

	return (ret);
}

static void *
huge_ralloc(void *ptr, size_t size, size_t oldsize)
{
	void *ret;
	size_t copysize;

	/* Avoid moving the allocation if the size class would not change. */
	if (oldsize > arena_maxclass &&
	    CHUNK_CEILING(size) == CHUNK_CEILING(oldsize)) {
#ifdef JEMALLOC_FILL
		if (opt_junk && size < oldsize) {
			memset((void *)((uintptr_t)ptr + size), 0x5a, oldsize
			    - size);
		} else if (opt_zero && size > oldsize) {
			memset((void *)((uintptr_t)ptr + oldsize), 0, size
			    - oldsize);
		}
#endif
		return (ptr);
	}

	/*
	 * If we get here, then size and oldsize are different enough that we
	 * need to use a different size class.  In that case, fall back to
	 * allocating new space and copying.
	 */
	ret = huge_malloc(size, false);
	if (ret == NULL)
		return (NULL);

	copysize = (size < oldsize) ? size : oldsize;
	memcpy(ret, ptr, copysize);
	idalloc(ptr);
	return (ret);
}

static void
huge_dalloc(void *ptr)
{
	extent_node_t *node, key;

	malloc_mutex_lock(&huge_mtx);

	/* Extract from tree of huge allocations. */
	key.addr = ptr;
	node = extent_tree_ad_search(&huge, &key);
	assert(node != NULL);
	assert(node->addr == ptr);
	extent_tree_ad_remove(&huge, node);

#ifdef JEMALLOC_STATS
	huge_ndalloc++;
	huge_allocated -= node->size;
#endif

	malloc_mutex_unlock(&huge_mtx);

	/* Unmap chunk. */
#ifdef JEMALLOC_FILL
#ifdef JEMALLOC_DSS
	if (opt_junk)
		memset(node->addr, 0x5a, node->size);
#endif
#endif
	chunk_dealloc(node->addr, node->size);

	base_node_dealloc(node);
}

#ifdef JEMALLOC_TRACE
static arena_t *
trace_arena(const void *ptr)
{
	arena_t *arena;
	arena_chunk_t *chunk;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if ((void *)chunk == ptr)
		arena = arenas[0];
	else
		arena = chunk->arena;

	return (arena);
}

static void
trace_flush(arena_t *arena)
{
	ssize_t err;

	err = write(arena->trace_fd, arena->trace_buf, arena->trace_buf_end);
	if (err == -1) {
		malloc_message("<jemalloc>",
		    ": write() failed during trace flush", "\n", "");
		abort();
	}
	arena->trace_buf_end = 0;
}

static void
trace_write(arena_t *arena, const char *s)
{
	unsigned i, slen, n;

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the trace buffer if it is full. */
		if (arena->trace_buf_end == TRACE_BUF_SIZE)
			trace_flush(arena);

		if (arena->trace_buf_end + slen <= TRACE_BUF_SIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = TRACE_BUF_SIZE - arena->trace_buf_end;
		}
		memcpy(&arena->trace_buf[arena->trace_buf_end], &s[i], n);
		arena->trace_buf_end += n;
		i += n;
	}
}

static unsigned
trace_get_tid(void)
{
	unsigned ret = trace_tid;

	if (ret == 0) {
		malloc_mutex_lock(&trace_mtx);
		trace_tid = trace_next_tid;
		trace_next_tid++;
		malloc_mutex_unlock(&trace_mtx);
		ret = trace_tid;

		/*
		 * Set trace_tsd to non-zero so that the cleanup function will
		 * be called upon thread exit.
		 */
		pthread_setspecific(trace_tsd, (void *)ret);
	}

	return (ret);
}

static void
malloc_trace_flush_all(void)
{
	unsigned i;

	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL) {
			malloc_mutex_lock(&arenas[i]->lock);
			trace_flush(arenas[i]);
			malloc_mutex_unlock(&arenas[i]->lock);
		}
	}
}

static void
trace_malloc(const void *ptr, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " m 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_calloc(const void *ptr, size_t number, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " c 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(number, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_posix_memalign(const void *ptr, size_t alignment, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " a 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(alignment, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_realloc(const void *ptr, const void *old_ptr, size_t size,
    size_t old_size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " r 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " 0x");
	trace_write(arena, umax2s((uintptr_t)old_ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(old_size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_free(const void *ptr, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " f 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(isalloc(ptr), 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_malloc_usable_size(size_t size, const void *ptr)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " s ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, " 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_thread_exit(void)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = choose_arena();

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " x\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_thread_cleanup(void *arg)
{

	trace_thread_exit();
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

static unsigned
malloc_ncpus(void)
{
	unsigned ret;
	long result;

	result = sysconf(_SC_NPROCESSORS_ONLN);
	if (result == -1) {
		/* Error. */
		ret = 1;
	}
	ret = (unsigned)result;

	return (ret);
}

/*
 * FreeBSD's pthreads implementation calls malloc(3), so the malloc
 * implementation has to take pains to avoid infinite recursion during
 * initialization.
 */
static inline bool
malloc_init(void)
{

	if (malloc_initialized == false)
		return (malloc_init_hard());

	return (false);
}

static bool
malloc_init_hard(void)
{
	unsigned i;
	int linklen;
	char buf[PATH_MAX + 1];
	const char *opts;
	arena_t *init_arenas[1];

	malloc_mutex_lock(&init_lock);
	if (malloc_initialized || malloc_initializer == pthread_self()) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		malloc_mutex_unlock(&init_lock);
		return (false);
	}
	if (malloc_initializer != (unsigned long)0) {
		/* Busy-wait until the initializing thread completes. */
		do {
			malloc_mutex_unlock(&init_lock);
			CPU_SPINWAIT;
			malloc_mutex_lock(&init_lock);
		} while (malloc_initialized == false);
		return (false);
	}

#ifdef DYNAMIC_PAGE_SHIFT
	/* Get page size. */
	{
		long result;

		result = sysconf(_SC_PAGESIZE);
		assert(result != -1);
		pagesize = (unsigned)result;

		/*
		 * We assume that pagesize is a power of 2 when calculating
		 * pagesize_mask and lg_pagesize.
		 */
		assert(((result - 1) & result) == 0);
		pagesize_mask = result - 1;
		lg_pagesize = ffs((int)result) - 1;
	}
#endif

	for (i = 0; i < 3; i++) {
		unsigned j;

		/* Get runtime configuration. */
		switch (i) {
		case 0:
			if ((linklen = readlink("/etc/jemalloc.conf", buf,
						sizeof(buf) - 1)) != -1) {
				/*
				 * Use the contents of the "/etc/jemalloc.conf"
				 * symbolic link's name.
				 */
				buf[linklen] = '\0';
				opts = buf;
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 1:
			if ((opts = getenv("JEMALLOC_OPTIONS")) != NULL) {
				/*
				 * Do nothing; opts is already initialized to
				 * the value of the JEMALLOC_OPTIONS
				 * environment variable.
				 */
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 2:
			if (malloc_options != NULL) {
				/*
				 * Use options that were compiled into the
				 * program.
				 */
				opts = malloc_options;
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		default:
			/* NOTREACHED */
			assert(false);
			buf[0] = '\0';
			opts = buf;
		}

		for (j = 0; opts[j] != '\0'; j++) {
			unsigned k, nreps;
			bool nseen;

			/* Parse repetition count, if any. */
			for (nreps = 0, nseen = false;; j++, nseen = true) {
				switch (opts[j]) {
					case '0': case '1': case '2': case '3':
					case '4': case '5': case '6': case '7':
					case '8': case '9':
						nreps *= 10;
						nreps += opts[j] - '0';
						break;
					default:
						goto MALLOC_OUT;
				}
			}
MALLOC_OUT:
			if (nseen == false)
				nreps = 1;

			for (k = 0; k < nreps; k++) {
				switch (opts[j]) {
				case 'a':
					opt_abort = false;
					break;
				case 'A':
					opt_abort = true;
					break;
				case 'c':
					if (opt_lg_cspace_max - 1 >
					    opt_lg_qspace_max &&
					    opt_lg_cspace_max >
					    LG_CACHELINE)
						opt_lg_cspace_max--;
					break;
				case 'C':
					if (opt_lg_cspace_max < PAGE_SHIFT
					    - 1)
						opt_lg_cspace_max++;
					break;
				case 'd':
					if (opt_lg_dirty_mult + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_dirty_mult++;
					break;
				case 'D':
					if (opt_lg_dirty_mult >= 0)
						opt_lg_dirty_mult--;
					break;
#ifdef JEMALLOC_TCACHE
				case 'g':
					if (opt_lg_tcache_gc_sweep >= 0)
						opt_lg_tcache_gc_sweep--;
					break;
				case 'G':
					if (opt_lg_tcache_gc_sweep + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_tcache_gc_sweep++;
					break;
				case 'h':
					opt_tcache = false;
					break;
				case 'H':
					opt_tcache = true;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'j':
					opt_junk = false;
					break;
				case 'J':
					opt_junk = true;
					break;
#endif
				case 'k':
					/*
					 * Chunks always require at least one
					 * header page, plus enough room to
					 * hold a run for the largest medium
					 * size class (one page more than the
					 * size).
					 */
					if ((1U << (opt_lg_chunk - 1)) >=
					    (2U << PAGE_SHIFT) + (1U <<
					    opt_lg_medium_max))
						opt_lg_chunk--;
					break;
				case 'K':
					if (opt_lg_chunk + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_chunk++;
					break;
				case 'm':
					if (opt_lg_medium_max > PAGE_SHIFT)
						opt_lg_medium_max--;
					break;
				case 'M':
					if (opt_lg_medium_max + 1 <
					    opt_lg_chunk)
						opt_lg_medium_max++;
					break;
				case 'n':
					opt_narenas_lshift--;
					break;
				case 'N':
					opt_narenas_lshift++;
					break;
				case 'p':
					opt_stats_print = false;
					break;
				case 'P':
					opt_stats_print = true;
					break;
				case 'q':
					if (opt_lg_qspace_max > LG_QUANTUM)
						opt_lg_qspace_max--;
					break;
				case 'Q':
					if (opt_lg_qspace_max + 1 <
					    opt_lg_cspace_max)
						opt_lg_qspace_max++;
					break;
#ifdef JEMALLOC_TRACE
				case 't':
					opt_trace = false;
					break;
				case 'T':
					opt_trace = true;
					break;
#endif
#ifdef JEMALLOC_SYSV
				case 'v':
					opt_sysv = false;
					break;
				case 'V':
					opt_sysv = true;
					break;
#endif
#ifdef JEMALLOC_XMALLOC
				case 'x':
					opt_xmalloc = false;
					break;
				case 'X':
					opt_xmalloc = true;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'z':
					opt_zero = false;
					break;
				case 'Z':
					opt_zero = true;
					break;
#endif
				default: {
					char cbuf[2];

					cbuf[0] = opts[j];
					cbuf[1] = '\0';
					malloc_message("<jemalloc>",
					    ": Unsupported character "
					    "in malloc options: '", cbuf,
					    "'\n");
				}
				}
			}
		}
	}

#ifdef JEMALLOC_TRACE
	if (opt_trace) {
		malloc_mutex_init(&trace_mtx);
		/* Flush trace buffers at exit. */
		atexit(malloc_trace_flush_all);
		/* Receive thread exit notifications. */
		if (pthread_key_create(&trace_tsd, trace_thread_cleanup) != 0) {
			malloc_message("<jemalloc>",
			    ": Error in pthread_key_create()\n", "", "");
			abort();
		}
	}
#endif
	if (opt_stats_print) {
		/* Print statistics at exit. */
		atexit(stats_print_atexit);
	}

	/* Register fork handlers. */
	pthread_atfork(jemalloc_prefork, jemalloc_postfork, jemalloc_postfork);

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
	    malloc_message("<jemalloc>: Too many small size classes (",
	        umax2s(mbin0, 10, line_buf), " > max 256)\n", "");
	    abort();
	}

	if (small_size2bin_init()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef JEMALLOC_TCACHE
	/* Compute incremental GC event threshold. */
	if (opt_lg_tcache_gc_sweep >= 0) {
		tcache_gc_incr = ((1U << opt_lg_tcache_gc_sweep) /
		    nbins) + (((1U << opt_lg_tcache_gc_sweep) % nbins == 0)
		    ? 0 : 1);
	} else
		tcache_gc_incr = 0;
#endif

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (1LU << opt_lg_chunk);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> PAGE_SHIFT);
	{
		size_t header_size;

		/*
		 * Compute the header size such that it is large enough to
		 * contain the page map.
		 */
		header_size = sizeof(arena_chunk_t) +
		    (sizeof(arena_chunk_map_t) * (chunk_npages - 1));
		arena_chunk_header_npages = (header_size >> PAGE_SHIFT) +
		    ((header_size & PAGE_MASK) != 0);
	}
	arena_maxclass = chunksize - (arena_chunk_header_npages <<
	    PAGE_SHIFT);

#ifdef JEMALLOC_STATS
	memset(&stats_chunks, 0, sizeof(chunk_stats_t));
#endif

	/* Various sanity checks that regard configuration. */
	assert(chunksize >= PAGE_SIZE);

	/* Initialize chunks data. */
	if (malloc_mutex_init(&huge_mtx)) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	extent_tree_ad_new(&huge);
#ifdef JEMALLOC_DSS
	if (malloc_mutex_init(&dss_mtx)) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	dss_base = sbrk(0);
	dss_prev = dss_base;
	dss_max = dss_base;
	extent_tree_szad_new(&dss_chunks_szad);
	extent_tree_ad_new(&dss_chunks_ad);
#endif
#ifdef JEMALLOC_STATS
	huge_nmalloc = 0;
	huge_ndalloc = 0;
	huge_allocated = 0;
#endif

	/* Initialize base allocation data structures. */
#ifdef JEMALLOC_STATS
	base_mapped = 0;
#endif
#ifdef JEMALLOC_DSS
	/*
	 * Allocate a base chunk here, since it doesn't actually have to be
	 * chunk-aligned.  Doing this before allocating any other chunks allows
	 * the use of space that would otherwise be wasted.
	 */
	base_pages_alloc(0);
#endif
	base_nodes = NULL;
	if (malloc_mutex_init(&base_mtx)) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas = 1;
	arenas = init_arenas;
	memset(arenas, 0, sizeof(arena_t *) * narenas);

	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * choose_arena_hard().
	 */
	arenas_extend(0);
	if (arenas[0] == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifndef NO_TLS
	/*
	 * Assign the initial arena to the initial thread, in order to avoid
	 * spurious creation of an extra arena if the application switches to
	 * threaded mode.
	 */
	arenas_map = arenas[0];
#endif

#ifdef JEMALLOC_TCACHE
	if (opt_tcache) {
		if (pthread_key_create(&tcache_tsd, tcache_thread_cleanup) !=
		    0) {
			malloc_message("<jemalloc>",
			    ": Error in pthread_key_create()\n", "", "");
			abort();
		}
	}
#endif

	malloc_mutex_init(&arenas_lock);

	/* Get number of CPUs. */
	malloc_initializer = pthread_self();
	malloc_mutex_unlock(&init_lock);
	ncpus = malloc_ncpus();
	malloc_mutex_lock(&init_lock);

	if (ncpus > 1) {
		/*
		 * For SMP systems, create more than one arena per CPU by
		 * default.
		 */
#ifdef JEMALLOC_TCACHE
		if (opt_tcache) {
			/*
			 * Only large object allocation/deallocation is
			 * guaranteed to acquire an arena mutex, so we can get
			 * away with fewer arenas than without thread caching.
			 */
			opt_narenas_lshift += 1;
		} else {
#endif
			/*
			 * All allocations must acquire an arena mutex, so use
			 * plenty of arenas.
			 */
			opt_narenas_lshift += 2;
#ifdef JEMALLOC_TCACHE
		}
#endif
	}

	/* Determine how many arenas to use. */
	narenas = ncpus;
	if (opt_narenas_lshift > 0) {
		if ((narenas << opt_narenas_lshift) > narenas)
			narenas <<= opt_narenas_lshift;
		/*
		 * Make sure not to exceed the limits of what base_alloc() can
		 * handle.
		 */
		if (narenas * sizeof(arena_t *) > chunksize)
			narenas = chunksize / sizeof(arena_t *);
	} else if (opt_narenas_lshift < 0) {
		if ((narenas >> -opt_narenas_lshift) < narenas)
			narenas >>= -opt_narenas_lshift;
		/* Make sure there is at least one arena. */
		if (narenas == 0)
			narenas = 1;
	}

#ifdef NO_TLS
	if (narenas > 1) {
		static const unsigned primes[] = {1, 3, 5, 7, 11, 13, 17, 19,
		    23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
		    89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149,
		    151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211,
		    223, 227, 229, 233, 239, 241, 251, 257, 263};
		unsigned nprimes, parenas;

		/*
		 * Pick a prime number of hash arenas that is more than narenas
		 * so that direct hashing of pthread_self() pointers tends to
		 * spread allocations evenly among the arenas.
		 */
		assert((narenas & 1) == 0); /* narenas must be even. */
		nprimes = (sizeof(primes) >> LG_SIZEOF_INT);
		parenas = primes[nprimes - 1]; /* In case not enough primes. */
		for (i = 1; i < nprimes; i++) {
			if (primes[i] > narenas) {
				parenas = primes[i];
				break;
			}
		}
		narenas = parenas;
	}
#endif

#ifndef NO_TLS
	next_arena = 0;
#endif

	/* Allocate and initialize arenas. */
	arenas = (arena_t **)base_alloc(sizeof(arena_t *) * narenas);
	if (arenas == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	/*
	 * Zero the array.  In practice, this should always be pre-zeroed,
	 * since it was just mmap()ed, but let's be sure.
	 */
	memset(arenas, 0, sizeof(arena_t *) * narenas);
	/* Copy the pointer to the one arena that was already initialized. */
	arenas[0] = init_arenas[0];

	malloc_initialized = true;
	malloc_mutex_unlock(&init_lock);
	return (false);
}

/*
 * End general internal functions.
 */
/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

JEMALLOC_ATTR(malloc)
void *
malloc(size_t size)
{
	void *ret;

	if (malloc_init()) {
		ret = NULL;
		goto OOM;
	}

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
#  ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_message("<jemalloc>",
				    ": Error in malloc(): invalid size 0\n", "",
				    "");
				abort();
			}
#  endif
			ret = NULL;
			goto RETURN;
		}
#endif
	}

	ret = imalloc(size);

OOM:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_message("<jemalloc>",
			    ": Error in malloc(): out of memory\n", "",
			    "");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_malloc(ret, size);
#endif
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret;
	void *result;

	if (malloc_init())
		result = NULL;
	else {
		if (size == 0) {
#ifdef JEMALLOC_SYSV
			if (opt_sysv == false)
#endif
				size = 1;
#ifdef JEMALLOC_SYSV
			else {
#  ifdef JEMALLOC_XMALLOC
				if (opt_xmalloc) {
					malloc_message("<jemalloc>",
					    ": Error in posix_memalign(): "
					    "invalid size 0\n", "", "");
					abort();
				}
#  endif
				result = NULL;
				*memptr = NULL;
				ret = 0;
				goto RETURN;
			}
#endif
		}

		/* Make sure that alignment is a large enough power of 2. */
		if (((alignment - 1) & alignment) != 0
		    || alignment < sizeof(void *)) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_message("<jemalloc>",
				    ": Error in posix_memalign(): "
				    "invalid alignment\n", "", "");
				abort();
			}
#endif
			result = NULL;
			ret = EINVAL;
			goto RETURN;
		}

		result = ipalloc(alignment, size);
	}

	if (result == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_message("<jemalloc>",
			": Error in posix_memalign(): out of memory\n",
			"", "");
			abort();
		}
#endif
		ret = ENOMEM;
		goto RETURN;
	}

	*memptr = result;
	ret = 0;

RETURN:
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_posix_memalign(result, alignment, size);
#endif
	return (ret);
}

JEMALLOC_ATTR(malloc)
void *
calloc(size_t num, size_t size)
{
	void *ret;
	size_t num_size;

	if (malloc_init()) {
		num_size = 0;
		ret = NULL;
		goto RETURN;
	}

	num_size = num * size;
	if (num_size == 0) {
#ifdef JEMALLOC_SYSV
		if ((opt_sysv == false) && ((num == 0) || (size == 0)))
#endif
			num_size = 1;
#ifdef JEMALLOC_SYSV
		else {
			ret = NULL;
			goto RETURN;
		}
#endif
	/*
	 * Try to avoid division here.  We know that it isn't possible to
	 * overflow during multiplication if neither operand uses any of the
	 * most significant half of the bits in a size_t.
	 */
	} else if (((num | size) & (SIZE_T_MAX << (sizeof(size_t) << 2)))
	    && (num_size / size != num)) {
		/* size_t overflow. */
		ret = NULL;
		goto RETURN;
	}

	ret = icalloc(num_size);

RETURN:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_message("<jemalloc>",
			    ": Error in calloc(): out of memory\n", "",
			    "");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_calloc(ret, num, size);
#endif
	return (ret);
}

void *
realloc(void *ptr, size_t size)
{
	void *ret;
#ifdef JEMALLOC_TRACE
	size_t old_size;
#endif

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
			if (ptr != NULL)
				idalloc(ptr);
			ret = NULL;
			goto RETURN;
		}
#endif
	}

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			old_size = isalloc(ptr);
#endif

		ret = iralloc(ptr, size);

		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_message("<jemalloc>",
				    ": Error in realloc(): out of "
				    "memory\n", "", "");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	} else {
		if (malloc_init())
			ret = NULL;
		else
			ret = imalloc(size);

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			old_size = 0;
#endif

		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_message("<jemalloc>",
				    ": Error in realloc(): out of "
				    "memory\n", "", "");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_realloc(ret, ptr, size, old_size);
#endif
	return (ret);
}

void
free(void *ptr)
{

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			trace_free(ptr, isalloc(ptr));
#endif
		idalloc(ptr);
	}
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

size_t
malloc_usable_size(const void *ptr)
{
	size_t ret;

	assert(ptr != NULL);
	ret = isalloc(ptr);

#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_malloc_usable_size(ret, ptr);
#endif
	return (ret);
}

#ifdef JEMALLOC_TCACHE
void
malloc_tcache_flush(void)
{
	tcache_t *tcache;

	tcache = tcache_tls;
	if (tcache == NULL)
		return;

	tcache_destroy(tcache);
	tcache_tls = NULL;
}
#endif

void
malloc_stats_print(const char *opts)
{
	char s[UMAX2S_BUFSIZE];
	bool general = true;
	bool bins = true;
	bool large = true;

	if (opts != NULL) {
		unsigned i;

		for (i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
				case 'g':
					general = false;
					break;
				case 'b':
					bins = false;
					break;
				case 'l':
					large = false;
					break;
				default:;
			}
		}
	}

	malloc_message("___ Begin jemalloc statistics ___\n", "", "", "");
	if (general) {
		malloc_message("Assertions ",
#ifdef NDEBUG
		    "disabled",
#else
		    "enabled",
#endif
		    "\n", "");
		malloc_message("Boolean JEMALLOC_OPTIONS: ",
		    opt_abort ? "A" : "a", "", "");
#ifdef JEMALLOC_TCACHE
		malloc_message(opt_tcache ? "H" : "h", "", "", "");
#endif
#ifdef JEMALLOC_FILL
		malloc_message(opt_junk ? "J" : "j", "", "", "");
#endif
		malloc_message("P", "", "", "");
#ifdef JEMALLOC_TRACE
		malloc_message(opt_trace ? "T" : "t", "", "", "");
#endif
#ifdef JEMALLOC_SYSV
		malloc_message(opt_sysv ? "V" : "v", "", "", "");
#endif
#ifdef JEMALLOC_XMALLOC
		malloc_message(opt_xmalloc ? "X" : "x", "", "", "");
#endif
#ifdef JEMALLOC_FILL
		malloc_message(opt_zero ? "Z" : "z", "", "", "");
#endif
		malloc_message("\n", "", "", "");

		malloc_message("CPUs: ", umax2s(ncpus, 10, s), "\n", "");
		malloc_message("Max arenas: ", umax2s(narenas, 10, s), "\n",
		    "");
		malloc_message("Pointer size: ", umax2s(sizeof(void *), 10, s),
		    "\n", "");
		malloc_message("Quantum size: ", umax2s(QUANTUM, 10, s), "\n",
		    "");
		malloc_message("Cacheline size (assumed): ",
		    umax2s(CACHELINE, 10, s), "\n", "");
		malloc_message("Subpage spacing: ", umax2s(SUBPAGE, 10, s),
		    "\n", "");
		malloc_message("Medium spacing: ", umax2s((1U << lg_mspace), 10,
		    s), "\n", "");
#ifdef JEMALLOC_TINY
		malloc_message("Tiny 2^n-spaced sizes: [", umax2s((1U <<
		    LG_TINY_MIN), 10, s), "..", "");
		malloc_message(umax2s((qspace_min >> 1), 10, s), "]\n", "", "");
#endif
		malloc_message("Quantum-spaced sizes: [", umax2s(qspace_min, 10,
		    s), "..", "");
		malloc_message(umax2s(qspace_max, 10, s), "]\n", "", "");
		malloc_message("Cacheline-spaced sizes: [",
		    umax2s(cspace_min, 10, s), "..", "");
		malloc_message(umax2s(cspace_max, 10, s), "]\n", "", "");
		malloc_message("Subpage-spaced sizes: [", umax2s(sspace_min, 10,
		    s), "..", "");
		malloc_message(umax2s(sspace_max, 10, s), "]\n", "", "");
		malloc_message("Medium sizes: [", umax2s(medium_min, 10, s),
		    "..", "");
		malloc_message(umax2s(medium_max, 10, s), "]\n", "", "");
		if (opt_lg_dirty_mult >= 0) {
			malloc_message(
			    "Min active:dirty page ratio per arena: ",
			    umax2s((1U << opt_lg_dirty_mult), 10, s), ":1\n",
			    "");
		} else {
			malloc_message(
			    "Min active:dirty page ratio per arena: N/A\n",
			    "", "", "");
		}
#ifdef JEMALLOC_TCACHE
		if (opt_tcache) {
			malloc_message("Thread cache GC sweep interval: ",
			    (tcache_gc_incr > 0) ?
			    umax2s((1U << opt_lg_tcache_gc_sweep), 10, s)
			    : "N/A",
			    "", "");
			malloc_message(" (increment interval: ",
			    (tcache_gc_incr > 0) ?
			    umax2s(tcache_gc_incr, 10, s)
			    : "N/A",
			    ")\n", "");
		}
#endif
		malloc_message("Chunk size: ", umax2s(chunksize, 10, s), "",
		    "");
		malloc_message(" (2^", umax2s(opt_lg_chunk, 10, s), ")\n", "");
	}

#ifdef JEMALLOC_STATS
	{
		size_t allocated, mapped;
		unsigned i;
		arena_t *arena;

		/* Calculate and print allocated/mapped stats. */

		/* arenas. */
		for (i = 0, allocated = 0; i < narenas; i++) {
			if (arenas[i] != NULL) {
				malloc_mutex_lock(&arenas[i]->lock);
				allocated += arenas[i]->stats.allocated_small;
				allocated += arenas[i]->stats.allocated_large;
				malloc_mutex_unlock(&arenas[i]->lock);
			}
		}

		/* huge/base. */
		malloc_mutex_lock(&huge_mtx);
		allocated += huge_allocated;
		mapped = stats_chunks.curchunks * chunksize;
		malloc_mutex_unlock(&huge_mtx);

		malloc_mutex_lock(&base_mtx);
		mapped += base_mapped;
		malloc_mutex_unlock(&base_mtx);

		malloc_printf("Allocated: %zu, mapped: %zu\n", allocated,
		    mapped);

		/* Print chunk stats. */
		{
			chunk_stats_t chunks_stats;

			malloc_mutex_lock(&huge_mtx);
			chunks_stats = stats_chunks;
			malloc_mutex_unlock(&huge_mtx);

			malloc_printf("chunks: nchunks   "
			    "highchunks    curchunks\n");
			malloc_printf("  %13llu%13lu%13lu\n",
			    chunks_stats.nchunks, chunks_stats.highchunks,
			    chunks_stats.curchunks);
		}

		/* Print chunk stats. */
		malloc_printf(
		    "huge: nmalloc      ndalloc    allocated\n");
		malloc_printf(" %12llu %12llu %12zu\n", huge_nmalloc,
		    huge_ndalloc, huge_allocated);

		/* Print stats for each arena. */
		for (i = 0; i < narenas; i++) {
			arena = arenas[i];
			if (arena != NULL) {
				malloc_printf("\narenas[%u]:\n", i);
				malloc_mutex_lock(&arena->lock);
				arena_stats_print(arena, bins, large);
				malloc_mutex_unlock(&arena->lock);
			}
		}
	}
#endif /* #ifdef JEMALLOC_STATS */
	malloc_message("--- End jemalloc statistics ---\n", "", "", "");
}

/*
 * End non-standard functions.
 */
/******************************************************************************/
/*
 * Begin library-private functions.
 */

/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().  These functions are only called if the program is
 * running in threaded mode, so there is no need to check whether the program
 * is threaded here.
 */

static void
jemalloc_prefork(void)
{
	bool again;
	unsigned i, j;
	arena_t *larenas[narenas], *tarenas[narenas];

	/* Acquire all mutexes in a safe order. */

	/*
	 * arenas_lock must be acquired after all of the arena mutexes, in
	 * order to avoid potential deadlock with arena_lock_balance[_hard]().
	 * Since arenas_lock protects the arenas array, the following code has
	 * to race with arenas_extend() callers until it succeeds in locking
	 * all arenas before locking arenas_lock.
	 */
	memset(larenas, 0, sizeof(arena_t *) * narenas);
	do {
		again = false;

		malloc_mutex_lock(&arenas_lock);
		for (i = 0; i < narenas; i++) {
			if (arenas[i] != larenas[i]) {
				memcpy(tarenas, arenas, sizeof(arena_t *) *
				    narenas);
				malloc_mutex_unlock(&arenas_lock);
				for (j = 0; j < narenas; j++) {
					if (larenas[j] != tarenas[j]) {
						larenas[j] = tarenas[j];
						malloc_mutex_lock(
						    &larenas[j]->lock);
					}
				}
				again = true;
				break;
			}
		}
	} while (again);

	malloc_mutex_lock(&base_mtx);

	malloc_mutex_lock(&huge_mtx);

#ifdef JEMALLOC_DSS
	malloc_mutex_lock(&dss_mtx);
#endif
}

static void
jemalloc_postfork(void)
{
	unsigned i;
	arena_t *larenas[narenas];

	/* Release all mutexes, now that fork() has completed. */

#ifdef JEMALLOC_DSS
	malloc_mutex_unlock(&dss_mtx);
#endif

	malloc_mutex_unlock(&huge_mtx);

	malloc_mutex_unlock(&base_mtx);

	memcpy(larenas, arenas, sizeof(arena_t *) * narenas);
	malloc_mutex_unlock(&arenas_lock);
	for (i = 0; i < narenas; i++) {
		if (larenas[i] != NULL)
			malloc_mutex_unlock(&larenas[i]->lock);
	}
}

/*
 * End library-private functions.
 */
/******************************************************************************/
