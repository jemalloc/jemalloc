#ifndef JEMALLOC_INTERNAL_ARENA_H
#define JEMALLOC_INTERNAL_ARENA_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena_decay_constants.h"
#include "jemalloc/internal/arena_stats.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/counter.h"
#include "jemalloc/internal/div.h"
#include "jemalloc/internal/ecache.h"
#include "jemalloc/internal/edata_cache.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/pa.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/stats.h"
#include "jemalloc/internal/ticker.h"

/******************************************************************************/
/* TYPES */
/******************************************************************************/

/* Default decay times in milliseconds. */
#define DIRTY_DECAY_MS_DEFAULT ZD(10 * 1000)
#define MUZZY_DECAY_MS_DEFAULT (0)
/* Maximum length of the arena name. */
#define ARENA_NAME_LEN 32

typedef enum {
	percpu_arena_mode_names_base = 0, /* Used for options processing. */

	/*
	 * *_uninit are used only during bootstrapping, and must correspond
	 * to initialized variant plus percpu_arena_mode_enabled_base.
	 */
	percpu_arena_uninit = 0,
	per_phycpu_arena_uninit = 1,

	/* All non-disabled modes must come after percpu_arena_disabled. */
	percpu_arena_disabled = 2,

	percpu_arena_mode_names_limit = 3, /* Used for options processing. */
	percpu_arena_mode_enabled_base = 3,

	percpu_arena = 3,
	per_phycpu_arena = 4 /* Hyper threads share arena. */
} percpu_arena_mode_t;

#define PERCPU_ARENA_ENABLED(m) ((m) >= percpu_arena_mode_enabled_base)
#define PERCPU_ARENA_DEFAULT percpu_arena_disabled

/*
 * When allocation_size >= oversize_threshold, use the dedicated huge arena
 * (unless have explicitly spicified arena index).  0 disables the feature.
 */
#define OVERSIZE_THRESHOLD_DEFAULT (8 << 20)

struct arena_config_s {
	/* extent hooks to be used for the arena */
	extent_hooks_t *extent_hooks;

	/*
	 * Use extent hooks for metadata (base) allocations when true.
	 */
	bool metadata_use_hooks;
};

typedef struct arena_config_s arena_config_t;

extern const arena_config_t arena_config_default;

/******************************************************************************/
/* STRUCTS */
/******************************************************************************/

struct arena_s {
	/*
	 * Number of threads currently assigned to this arena.  Each thread has
	 * two distinct assignments, one for application-serving allocation, and
	 * the other for internal metadata allocation.  Internal metadata must
	 * not be allocated from arenas explicitly created via the arenas.create
	 * mallctl, because the arena.<i>.reset mallctl indiscriminately
	 * discards all allocations for the affected arena.
	 *
	 *   0: Application allocation.
	 *   1: Internal metadata allocation.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t nthreads[2];

	/* Next bin shard for binding new threads. Synchronization: atomic. */
	atomic_u_t binshard_next;

	/*
	 * When percpu_arena is enabled, to amortize the cost of reading /
	 * updating the current CPU id, track the most recent thread accessing
	 * this arena, and only read CPU if there is a mismatch.
	 */
	tsdn_t *last_thd;

	/* Synchronization: internal. */
	arena_stats_t stats;

	/*
	 * List of cache_bin_array_descriptors for extant threads associated
	 * with this arena.  Stats from these are merged incrementally, and at
	 * exit if opt_stats_print is enabled.
	 *
	 * Synchronization: cache_bin_array_descriptor_ql_mtx.
	 */
	ql_head(cache_bin_array_descriptor_t) cache_bin_array_descriptor_ql;
	malloc_mutex_t cache_bin_array_descriptor_ql_mtx;

	/*
	 * Represents a dss_prec_t, but atomically.
	 *
	 * Synchronization: atomic.
	 */
	atomic_u_t dss_prec;

	/*
	 * Extant large allocations.
	 *
	 * Synchronization: large_mtx.
	 */
	edata_list_active_t large;
	/* Synchronizes all large allocation/update/deallocation. */
	malloc_mutex_t large_mtx;

	/* The page-level allocator shard this arena uses. */
	pa_shard_t pa_shard;

	/*
	 * A cached copy of base->ind.  This can get accessed on hot paths;
	 * looking it up in base requires an extra pointer hop / cache miss.
	 */
	unsigned ind;

	/*
	 * Base allocator, from which arena metadata are allocated.
	 *
	 * Synchronization: internal.
	 */
	base_t *base;
	/* Used to determine uptime.  Read-only after initialization. */
	nstime_t create_time;

	/* The name of the arena. */
	char name[ARENA_NAME_LEN];

	/*
	 * The arena is allocated alongside its bins; really this is a
	 * dynamically sized array determined by the binshard settings.
	 * Enforcing cacheline-alignment to minimize the number of cachelines
	 * touched on the hot paths.
	 */
	JEMALLOC_WARN_ON_USAGE(
	    "Do not use this field directly. "
	    "Use `arena_get_bin` instead.")
	JEMALLOC_ALIGNED(CACHELINE)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
	bin_t all_bins[];
#else
	bin_t all_bins[0];
#endif
};

/******************************************************************************/
/* EXTERNS */
/******************************************************************************/

/*
 * When the amount of pages to be purged exceeds this amount, deferred purge
 * should happen.
 */
#define ARENA_DEFERRED_PURGE_NPAGES_THRESHOLD UINT64_C(1024)

extern ssize_t opt_dirty_decay_ms;
extern ssize_t opt_muzzy_decay_ms;

extern percpu_arena_mode_t opt_percpu_arena;
extern const char *const   percpu_arena_mode_names[];

extern div_info_t arena_binind_div_info[SC_NBINS];

extern emap_t arena_emap_global;

extern size_t opt_oversize_threshold;
extern size_t oversize_threshold;

extern bool      opt_huge_arena_pac_thp;
extern pac_thp_t huge_arena_pac_thp;

/*
 * arena_bin_offsets[binind] is the offset of the first bin shard for size class
 * binind.
 */
extern uint32_t arena_bin_offsets[SC_NBINS];

void arena_basic_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy);
void arena_stats_merge(tsdn_t *tsdn, arena_t *arena, unsigned *nthreads,
    const char **dss, ssize_t *dirty_decay_ms, ssize_t *muzzy_decay_ms,
    size_t *nactive, size_t *ndirty, size_t *nmuzzy, arena_stats_t *astats,
    bin_stats_data_t *bstats, arena_stats_large_t *lstats, pac_estats_t *estats,
    hpa_shard_stats_t *hpastats);
void arena_handle_deferred_work(tsdn_t *tsdn, arena_t *arena);
edata_t *arena_extent_alloc_large(
    tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment, bool zero);
void arena_extent_dalloc_large_prep(
    tsdn_t *tsdn, arena_t *arena, const edata_t *edata);
void arena_extent_ralloc_large_shrink(
    tsdn_t *tsdn, arena_t *arena, const edata_t *edata, size_t oldusize);
void arena_extent_ralloc_large_expand(
    tsdn_t *tsdn, arena_t *arena, const edata_t *edata, size_t oldusize);
bool arena_decay_ms_set(
    tsdn_t *tsdn, arena_t *arena, extent_state_t state, ssize_t decay_ms);
ssize_t arena_decay_ms_get(arena_t *arena, extent_state_t state);
void    arena_decay(
       tsdn_t *tsdn, arena_t *arena, bool is_background_thread, bool all);
uint64_t       arena_time_until_deferred(tsdn_t *tsdn, arena_t *arena);
void           arena_do_deferred_work(tsdn_t *tsdn, arena_t *arena);
void           arena_reset(tsd_t *tsd, arena_t *arena);
void           arena_destroy(tsd_t *tsd, arena_t *arena);
cache_bin_sz_t arena_ptr_array_fill_small(tsdn_t *tsdn, arena_t *arena,
    szind_t binind, cache_bin_ptr_array_t *arr, const cache_bin_sz_t nfill_min,
    const cache_bin_sz_t nfill_max, cache_bin_stats_t merge_stats);

void *arena_malloc_hard(tsdn_t *tsdn, arena_t *arena, size_t size, szind_t ind,
    bool zero, bool slab);
void  arena_prof_promote(
     tsdn_t *tsdn, void *ptr, size_t usize, size_t bumped_usize);
size_t arena_prof_demote(tsdn_t *tsdn, edata_t *edata, const void *ptr);
void arena_slab_dalloc(tsdn_t *tsdn, arena_t *arena, edata_t *slab);

void  arena_dalloc_small(tsdn_t *tsdn, void *ptr);
void  arena_ptr_array_flush(tsd_t *tsd, szind_t binind,
     cache_bin_ptr_array_t *arr, unsigned nflush, bool small,
     arena_t *stats_arena, cache_bin_stats_t merge_stats);
bool  arena_ralloc_no_move(tsdn_t *tsdn, void *ptr, size_t oldsize, size_t size,
     size_t extra, bool zero, size_t *newsize);
dss_prec_t      arena_dss_prec_get(const arena_t *arena);
ehooks_t       *arena_get_ehooks(const arena_t *arena);
extent_hooks_t *arena_set_extent_hooks(
    tsd_t *tsd, arena_t *arena, extent_hooks_t *extent_hooks);
bool    arena_dss_prec_set(arena_t *arena, dss_prec_t dss_prec);
void    arena_name_get(const arena_t *arena, char *name);
void    arena_name_set(arena_t *arena, const char *name);
ssize_t arena_dirty_decay_ms_default_get(void);
bool    arena_dirty_decay_ms_default_set(ssize_t decay_ms);
ssize_t arena_muzzy_decay_ms_default_get(void);
bool    arena_muzzy_decay_ms_default_set(ssize_t decay_ms);
bool    arena_retain_grow_limit_get_set(
       tsd_t *tsd, arena_t *arena, size_t *old_limit, size_t *new_limit);
unsigned arena_nthreads_get(const arena_t *arena, bool internal);
void     arena_nthreads_inc(arena_t *arena, bool internal);
void     arena_nthreads_dec(arena_t *arena, bool internal);
arena_t *arena_new(tsdn_t *tsdn, unsigned ind, const arena_config_t *config);
bool     arena_init_huge(tsdn_t *tsdn, arena_t *a0);
bool     arena_ind_is_huge(unsigned ind);
arena_t *arena_choose_huge(tsd_t *tsd);
bool   arena_boot(sc_data_t *sc_data, base_t *base, bool hpa);
void  *arena_locality_hint(tsdn_t *tsdn, arena_t *arena, szind_t szind);
void   arena_cache_bin_array_register(tsdn_t *tsdn, arena_t *arena,
       cache_bin_array_descriptor_t *desc);
void   arena_cache_bin_array_unregister(tsdn_t *tsdn, arena_t *arena,
       cache_bin_array_descriptor_t *desc);
void   arena_cache_bin_array_postfork_child(arena_t *arena,
       cache_bin_array_descriptor_t *desc_or_null);
void   arena_cache_bins_stats_merge(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork0(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork1(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork2(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork3(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork4(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork5(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork6(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork7(tsdn_t *tsdn, arena_t *arena);
void   arena_prefork8(tsdn_t *tsdn, arena_t *arena);
void   arena_postfork_parent(tsdn_t *tsdn, arena_t *arena);
void   arena_postfork_child(tsdn_t *tsdn, arena_t *arena,
       cache_bin_array_descriptor_t *surviving_desc);

#endif /* JEMALLOC_INTERNAL_ARENA_H */
