#ifndef JEMALLOC_INTERNAL_TCACHE_H
#define JEMALLOC_INTERNAL_TCACHE_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/cache_bin.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/thread_event_registry.h"
#include "jemalloc/internal/ticker.h"

/* Forward decl; only base_t * is used as a pointer arg below. */
typedef struct base_s base_t;

/******************************************************************************/
/* TYPES */
/******************************************************************************/

typedef struct tcache_slow_s tcache_slow_t;
typedef struct tcache_s      tcache_t;
typedef struct tcaches_s     tcaches_t;

/* Used in TSD static initializer only. Real init in tsd_tcache_data_init(). */
#define TCACHE_ZERO_INITIALIZER                                                \
	{ 0 }
#define TCACHE_SLOW_ZERO_INITIALIZER                                           \
	{                                                                      \
		{ 0 }                                                          \
	}

/* Used in TSD static initializer only. Will be initialized to opt_tcache. */
#define TCACHE_ENABLED_ZERO_INITIALIZER false

/* Used for explicit tcache only. Means flushed but not destroyed. */
/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
#define TCACHES_ELM_NEED_REINIT ((tcache_t *)(uintptr_t)1)

#define TCACHE_LG_MAXCLASS_LIMIT LG_USIZE_GROW_SLOW_THRESHOLD
#define TCACHE_MAXCLASS_LIMIT ((size_t)1 << TCACHE_LG_MAXCLASS_LIMIT)
#define TCACHE_NBINS_MAX                                                       \
	(SC_NBINS                                                              \
	    + SC_NGROUP * (TCACHE_LG_MAXCLASS_LIMIT - SC_LG_LARGE_MINCLASS)    \
	    + 1)
#define TCACHE_GC_NEIGHBOR_LIMIT ((uintptr_t)1 << 21)       /* 2M */
#define TCACHE_GC_INTERVAL_NS ((uint64_t)10 * KQU(1000000)) /* 10ms */
#define TCACHE_GC_SMALL_NBINS_MAX ((SC_NBINS > 8) ? (SC_NBINS >> 3) : 1)
#define TCACHE_GC_LARGE_NBINS_MAX 1

/******************************************************************************/
/* STRUCTS */
/******************************************************************************/

/*
 * The tcache state is split into the slow and hot path data.  Each has a
 * pointer to the other, and the data always comes in pairs.  The layout of each
 * of them varies in practice; tcache_slow lives in the TSD for the automatic
 * tcache, and as part of a dynamic allocation for manual allocations.  Keeping
 * a pointer to tcache_slow lets us treat these cases uniformly, rather than
 * splitting up the tcache [de]allocation code into those paths called with the
 * TSD tcache and those called with a manual tcache.
 */

struct tcache_slow_s {
	/*
	 * The descriptor lets the arena find our cache bins without seeing the
	 * tcache definition.  This enables arenas to aggregate stats across
	 * tcaches without having a tcache dependency.
	 */
	cache_bin_array_descriptor_t cache_bin_array_descriptor;

	/* The arena this tcache is associated with. */
	arena_t *arena;
	/* The number of bins activated in the tcache. */
	unsigned tcache_nbins;
	/* Last time GC has been performed.  */
	nstime_t last_gc_time;
	/* Next bin to GC. */
	szind_t next_gc_bin;
	szind_t next_gc_bin_small;
	szind_t next_gc_bin_large;
	/* For small bins, help determine how many items to fill at a time. */
	cache_bin_fill_ctl_t bin_fill_ctl_do_not_access_directly[SC_NBINS];
	/* For small bins, whether has been refilled since last GC. */
	bool bin_refilled[SC_NBINS];
	/*
	 * For small bins, the number of items we can pretend to flush before
	 * actually flushing.
	 */
	uint8_t bin_flush_delay_items[SC_NBINS];
	/*
	 * The start of the allocation containing the dynamic allocation for
	 * either the cache bins alone, or the cache bin memory as well as this
	 * tcache_slow_t and its associated tcache_t.
	 */
	void *dyn_alloc;

	/* The associated bins. */
	tcache_t *tcache;
};

struct tcache_s {
	tcache_slow_t *tcache_slow;
	cache_bin_t    bins[TCACHE_NBINS_MAX];
};

/* Linkage for list of available (previously used) explicit tcache IDs. */
struct tcaches_s {
	union {
		tcache_t  *tcache;
		tcaches_t *next;
	};
};

/******************************************************************************/
/* EXTERNS */
/******************************************************************************/

extern bool     opt_tcache;
extern size_t   opt_tcache_max;
extern ssize_t  opt_lg_tcache_nslots_mul;
extern unsigned opt_tcache_nslots_small_min;
extern unsigned opt_tcache_nslots_small_max;
extern unsigned opt_tcache_nslots_large;
extern ssize_t  opt_lg_tcache_shift;
extern size_t   opt_tcache_gc_incr_bytes;
extern size_t   opt_tcache_gc_delay_bytes;
extern unsigned opt_lg_tcache_flush_small_div;
extern unsigned opt_lg_tcache_flush_large_div;

/*
 * Number of tcache bins.  There are SC_NBINS small-object bins, plus 0 or more
 * large-object bins.  This is only used during threads initialization and
 * changing it will not reflect on initialized threads as expected.  Thus,
 * it should not be changed on the fly.  To change the number of tcache bins
 * in use, refer to tcache_nbins of each tcache.
 */
extern unsigned global_do_not_change_tcache_nbins;

/*
 * Maximum cached size class.  Same as above, this is only used during threads
 * initialization and should not be changed.  To change the maximum cached size
 * class, refer to tcache_max of each tcache.
 */
extern size_t global_do_not_change_tcache_maxclass;

/*
 * Explicit tcaches, managed via the tcache.{create,flush,destroy} mallctls and
 * usable via the MALLOCX_TCACHE() flag.  The automatic per thread tcaches are
 * completely disjoint from this data structure.  tcaches starts off as a sparse
 * array, so it has no physical memory footprint until individual pages are
 * touched.  This allows the entire array to be allocated the first time an
 * explicit tcache is created without a disproportionate impact on memory usage.
 */
extern tcaches_t *tcaches;

size_t tcache_salloc(tsdn_t *tsdn, const void *ptr);
void  *tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
     cache_bin_t *cache_bin, szind_t binind, bool *tcache_success);

void tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache,
    cache_bin_t *cache_bin, szind_t binind, unsigned rem);
void tcache_bin_flush_large(tsd_t *tsd, tcache_t *tcache,
    cache_bin_t *cache_bin, szind_t binind, unsigned rem);
void tcache_bin_flush_stashed(tsd_t *tsd, tcache_t *tcache,
    cache_bin_t *cache_bin, szind_t binind, bool is_small);
bool tcache_bin_info_default_init(
    const char *bin_settings_segment_cur, size_t len_left);
bool tcache_bins_ncached_max_write(tsd_t *tsd, char *settings, size_t len);
bool tcache_bin_ncached_max_read(
    tsd_t *tsd, size_t bin_size, cache_bin_sz_t *ncached_max);
void tcache_arena_reassociate(
    tsdn_t *tsdn, tcache_slow_t *tcache_slow, arena_t *arena);
tcache_t *tcache_create_explicit(tsd_t *tsd);
bool      thread_tcache_max_set(tsd_t *tsd, size_t tcache_max);
void      tcache_cleanup(tsd_t *tsd);
bool      tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind);
void      tcaches_flush(tsd_t *tsd, unsigned ind);
void      tcaches_destroy(tsd_t *tsd, unsigned ind);
bool      tcache_boot(tsdn_t *tsdn, base_t *base);
void      tcache_arena_associate(
         tsdn_t *tsdn, tcache_slow_t *tcache_slow, arena_t *arena);
cache_bin_array_descriptor_t *tcache_postfork_arena_descriptor(
         tsdn_t *tsdn, arena_t *arena);
void tcache_prefork(tsdn_t *tsdn);
void tcache_postfork_parent(tsdn_t *tsdn);
void tcache_postfork_child(tsdn_t *tsdn);
void tcache_flush(tsd_t *tsd);
bool tsd_tcache_enabled_data_init(tsd_t *tsd);
void tcache_enabled_set(tsd_t *tsd, bool enabled);

extern void *(*JET_MUTABLE tcache_stack_alloc)(tsdn_t *tsdn, size_t size,
    size_t alignment);

void tcache_assert_initialized(tcache_t *tcache);

extern te_base_cb_t tcache_gc_te_handler;

#endif /* JEMALLOC_INTERNAL_TCACHE_H */
