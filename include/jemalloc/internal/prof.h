#ifndef JEMALLOC_INTERNAL_PROF_H
#define JEMALLOC_INTERNAL_PROF_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/ckh.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prng.h"
#include "jemalloc/internal/prof_hook.h"
#include "jemalloc/internal/rb.h"
#include "jemalloc/internal/thread_event_registry.h"

/* Forward decl; only base_t * is used as a pointer arg below. */
typedef struct base_s base_t;

/******************************************************************************/
/* TYPES */
/******************************************************************************/

typedef struct prof_bt_s     prof_bt_t;
typedef struct prof_cnt_s    prof_cnt_t;
typedef struct prof_tctx_s   prof_tctx_t;
typedef struct prof_info_s   prof_info_t;
typedef struct prof_gctx_s   prof_gctx_t;
typedef struct prof_tdata_s  prof_tdata_t;
typedef struct prof_recent_s prof_recent_t;

/* Option defaults. */
#ifdef JEMALLOC_PROF
#	define PROF_PREFIX_DEFAULT "jeprof"
#else
#	define PROF_PREFIX_DEFAULT ""
#endif
#define LG_PROF_SAMPLE_DEFAULT 19
#define LG_PROF_INTERVAL_DEFAULT -1

/*
 * Hard limit on stack backtrace depth.  The version of prof_backtrace() that
 * is based on __builtin_return_address() necessarily has a hard-coded number
 * of backtrace frame handlers, and should be kept in sync with this setting.
 */
#ifdef JEMALLOC_PROF_GCC
#	define PROF_BT_MAX_LIMIT 256
#else
#	define PROF_BT_MAX_LIMIT UINT_MAX
#endif
#define PROF_BT_MAX_DEFAULT 128

/* Initial hash table size. */
#define PROF_CKH_MINITEMS 64

/* Size of memory buffer to use when writing dump files. */
#ifndef JEMALLOC_PROF
/* Minimize memory bloat for non-prof builds. */
#	define PROF_DUMP_BUFSIZE 1
#elif defined(JEMALLOC_DEBUG)
/* Use a small buffer size in debug build, mainly to facilitate testing. */
#	define PROF_DUMP_BUFSIZE 16
#else
#	define PROF_DUMP_BUFSIZE 65536
#endif

/* Size of size class related tables */
#ifdef JEMALLOC_PROF
#	define PROF_SC_NSIZES SC_NSIZES
#else
/* Minimize memory bloat for non-prof builds. */
#	define PROF_SC_NSIZES 1
#endif

/* Size of stack-allocated buffer used by prof_printf(). */
#define PROF_PRINTF_BUFSIZE 128

/*
 * Number of mutexes shared among all gctx's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define PROF_NCTX_LOCKS 1024

/*
 * Number of mutexes shared among all tdata's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define PROF_NTDATA_LOCKS 256

/* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
#	define PROF_DUMP_FILENAME_LEN (PATH_MAX + 1)
#else
#	define PROF_DUMP_FILENAME_LEN 1
#endif

/* Default number of recent allocations to record. */
#define PROF_RECENT_ALLOC_MAX_DEFAULT 0

/* Thread name storage size limit. */
#define PROF_THREAD_NAME_MAX_LEN 16

/*
 * Minimum required alignment for sampled allocations. Over-aligning sampled
 * allocations allows us to quickly identify them on the dalloc path without
 * resorting to metadata lookup.
 */
#define PROF_SAMPLE_ALIGNMENT PAGE
#define PROF_SAMPLE_ALIGNMENT_MASK PAGE_MASK

/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
#define PROF_TCTX_SENTINEL ((prof_tctx_t *)((uintptr_t)1U))

/******************************************************************************/
/* STRUCTS */
/******************************************************************************/

struct prof_bt_s {
	/* Backtrace, stored as len program counters. */
	void   **vec;
	unsigned len;
};

#ifdef JEMALLOC_PROF_LIBGCC
/* Data structure passed to libgcc _Unwind_Backtrace() callback functions. */
typedef struct {
	void    **vec;
	unsigned *len;
	unsigned  max;
} prof_unwind_data_t;
#endif

struct prof_cnt_s {
	/* Profiling counters. */
	uint64_t curobjs;
	uint64_t curobjs_shifted_unbiased;
	uint64_t curbytes;
	uint64_t curbytes_unbiased;
	uint64_t accumobjs;
	uint64_t accumobjs_shifted_unbiased;
	uint64_t accumbytes;
	uint64_t accumbytes_unbiased;
};

typedef enum {
	prof_tctx_state_initializing,
	prof_tctx_state_nominal,
	prof_tctx_state_dumping,
	prof_tctx_state_purgatory /* Dumper must finish destroying. */
} prof_tctx_state_t;

struct prof_tctx_s {
	/* Thread data for thread that performed the allocation. */
	prof_tdata_t *tdata;

	/*
	 * Copy of tdata->thr_{uid,discrim}, necessary because tdata may be
	 * defunct during teardown.
	 */
	uint64_t thr_uid;
	uint64_t thr_discrim;

	/*
	 * Reference count of how many times this tctx object is referenced in
	 * recent allocation / deallocation records, protected by tdata->lock.
	 */
	uint64_t recent_count;

	/* Profiling counters, protected by tdata->lock. */
	prof_cnt_t cnts;

	/* Associated global context. */
	prof_gctx_t *gctx;

	/*
	 * UID that distinguishes multiple tctx's created by the same thread,
	 * but coexisting in gctx->tctxs.  There are two ways that such
	 * coexistence can occur:
	 * - A dumper thread can cause a tctx to be retained in the purgatory
	 *   state.
	 * - Although a single "producer" thread must create all tctx's which
	 *   share the same thr_uid, multiple "consumers" can each concurrently
	 *   execute portions of prof_tctx_destroy().  prof_tctx_destroy() only
	 *   gets called once each time cnts.cur{objs,bytes} drop to 0, but this
	 *   threshold can be hit again before the first consumer finishes
	 *   executing prof_tctx_destroy().
	 */
	uint64_t tctx_uid;

	/* Linkage into gctx's tctxs. */
	rb_node(prof_tctx_t) tctx_link;

	/*
	 * True during prof_alloc_prep()..prof_malloc_sample_object(), prevents
	 * sample vs destroy race.
	 */
	bool prepared;

	/* Current dump-related state, protected by gctx->lock. */
	prof_tctx_state_t state;

	/*
	 * Copy of cnts snapshotted during early dump phase, protected by
	 * dump_mtx.
	 */
	prof_cnt_t dump_cnts;
};
typedef rb_tree(prof_tctx_t) prof_tctx_tree_t;

struct prof_info_s {
	/* Time when the allocation was made. */
	nstime_t alloc_time;
	/* Points to the prof_tctx_t corresponding to the allocation. */
	prof_tctx_t *alloc_tctx;
	/* Allocation request size. */
	size_t alloc_size;
};

struct prof_gctx_s {
	/* Protects nlimbo, cnt_summed, and tctxs. */
	malloc_mutex_t *lock;

	/*
	 * Number of threads that currently cause this gctx to be in a state of
	 * limbo due to one of:
	 *   - Initializing this gctx.
	 *   - Initializing per thread counters associated with this gctx.
	 *   - Preparing to destroy this gctx.
	 *   - Dumping a heap profile that includes this gctx.
	 * nlimbo must be 1 (single destroyer) in order to safely destroy the
	 * gctx.
	 */
	unsigned nlimbo;

	/*
	 * Tree of profile counters, one for each thread that has allocated in
	 * this context.
	 */
	prof_tctx_tree_t tctxs;

	/* Linkage for tree of contexts to be dumped. */
	rb_node(prof_gctx_t) dump_link;

	/* Temporary storage for summation during dump. */
	prof_cnt_t cnt_summed;

	/* Associated backtrace. */
	prof_bt_t bt;

	/* Backtrace vector, variable size, referred to by bt. */
	void *vec[1];
};
typedef rb_tree(prof_gctx_t) prof_gctx_tree_t;

struct prof_tdata_s {
	malloc_mutex_t *lock;

	/* Monotonically increasing unique thread identifier. */
	uint64_t thr_uid;

	/*
	 * Monotonically increasing discriminator among tdata structures
	 * associated with the same thr_uid.
	 */
	uint64_t thr_discrim;

	rb_node(prof_tdata_t) tdata_link;

	/*
	 * Counter used to initialize prof_tctx_t's tctx_uid.  No locking is
	 * necessary when incrementing this field, because only one thread ever
	 * does so.
	 */
	uint64_t tctx_uid_next;

	/*
	 * Hash of (prof_bt_t *)-->(prof_tctx_t *).  Each thread tracks
	 * backtraces for which it has non-zero allocation/deallocation counters
	 * associated with thread-specific prof_tctx_t objects.  Other threads
	 * may write to prof_tctx_t contents when freeing associated objects.
	 */
	ckh_t bt2tctx;

	/* Included in heap profile dumps if has content. */
	char thread_name[PROF_THREAD_NAME_MAX_LEN];

	/* State used to avoid dumping while operating on prof internals. */
	bool enq;
	bool enq_idump;
	bool enq_gdump;

	/*
	 * Set to true during an early dump phase for tdata's which are
	 * currently being dumped.  New threads' tdata's have this initialized
	 * to false so that they aren't accidentally included in later dump
	 * phases.
	 */
	bool dumping;

	/*
	 * True if profiling is active for this tdata's thread
	 * (thread.prof.active mallctl).
	 */
	bool active;

	bool attached;
	bool expired;

	/* Temporary storage for summation during dump. */
	prof_cnt_t cnt_summed;

	/* Backtrace vector, used for calls to prof_backtrace(). */
	void **vec;
};
typedef rb_tree(prof_tdata_t) prof_tdata_tree_t;

struct prof_recent_s {
	nstime_t alloc_time;
	nstime_t dalloc_time;

	ql_elm(prof_recent_t) link;
	size_t       size;
	size_t       usize;
	atomic_p_t   alloc_edata; /* NULL means allocation has been freed. */
	prof_tctx_t *alloc_tctx;
	prof_tctx_t *dalloc_tctx;
};

/******************************************************************************/
/* EXTERNS */
/******************************************************************************/

extern bool     opt_prof;
extern bool     opt_prof_active;
extern bool     opt_prof_thread_active_init;
extern unsigned opt_prof_bt_max;
extern size_t   opt_lg_prof_sample; /* Mean bytes between samples. */
extern ssize_t opt_lg_prof_interval;    /* lg(prof_interval). */
extern bool    opt_prof_gdump;          /* High-water memory dumping. */
extern bool    opt_prof_final;          /* Final profile dumping. */
extern bool    opt_prof_leak;           /* Dump leak summary at exit. */
extern bool    opt_prof_leak_error; /* Exit with error code if memory leaked */
extern bool    opt_prof_accum;      /* Report cumulative bytes. */
extern bool    opt_prof_log;        /* Turn logging on at boot. */
extern char    opt_prof_prefix[
/* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
    PATH_MAX +
#endif
    1];
extern bool opt_prof_unbias;

/* Include pid namespace in profile file names. */
extern bool opt_prof_pid_namespace;

/* For recording recent allocations */
extern ssize_t opt_prof_recent_alloc_max;

/* Whether to use thread name provided by the system or by mallctl. */
extern bool opt_prof_sys_thread_name;

/* Whether to record per size class counts and request size totals. */
extern bool opt_prof_stats;

/* Accessed via prof_active_[gs]et{_unlocked,}(). */
extern bool prof_active_state;

/* Accessed via prof_gdump_[gs]et{_unlocked,}(). */
extern bool prof_gdump_val;

/* Profile dump interval, measured in bytes allocated. */
extern uint64_t prof_interval;

/*
 * Initialized as opt_lg_prof_sample, and potentially modified during profiling
 * resets.
 */
extern size_t lg_prof_sample;

extern bool prof_booted;

void                  prof_backtrace_hook_set(prof_backtrace_hook_t hook);
prof_backtrace_hook_t prof_backtrace_hook_get(void);

void             prof_dump_hook_set(prof_dump_hook_t hook);
prof_dump_hook_t prof_dump_hook_get(void);

void               prof_sample_hook_set(prof_sample_hook_t hook);
prof_sample_hook_t prof_sample_hook_get(void);

void                    prof_sample_free_hook_set(prof_sample_free_hook_t hook);
prof_sample_free_hook_t prof_sample_free_hook_get(void);

/* Functions only accessed in prof_inlines.h */
prof_tdata_t *prof_tdata_init(tsd_t *tsd);
prof_tdata_t *prof_tdata_reinit(tsd_t *tsd, prof_tdata_t *tdata);

void prof_alloc_rollback(tsd_t *tsd, prof_tctx_t *tctx);
void prof_malloc_sample_object(
    tsd_t *tsd, const void *ptr, size_t size, size_t usize, prof_tctx_t *tctx);
void prof_free_sampled_object(
    tsd_t *tsd, const void *ptr, size_t usize, prof_info_t *prof_info);
prof_tctx_t *prof_tctx_create(tsd_t *tsd);
void         prof_idump(tsdn_t *tsdn);
bool         prof_mdump(tsd_t *tsd, const char *filename);
void         prof_gdump(tsdn_t *tsdn);

void        prof_tdata_cleanup(tsd_t *tsd);
bool        prof_active_get(tsdn_t *tsdn);
bool        prof_active_set(tsdn_t *tsdn, bool active);
const char *prof_thread_name_get(tsd_t *tsd);
int         prof_thread_name_set(tsd_t *tsd, const char *thread_name);
bool        prof_thread_active_get(tsd_t *tsd);
bool        prof_thread_active_set(tsd_t *tsd, bool active);
bool        prof_thread_active_init_get(tsdn_t *tsdn);
bool        prof_thread_active_init_set(tsdn_t *tsdn, bool active_init);
bool        prof_gdump_get(tsdn_t *tsdn);
bool        prof_gdump_set(tsdn_t *tsdn, bool active);
void        prof_boot0(void);
void        prof_boot1(void);
bool        prof_boot2(tsd_t *tsd, base_t *base);
void        prof_prefork0(tsdn_t *tsdn);
void        prof_prefork1(tsdn_t *tsdn);
void        prof_postfork_parent(tsdn_t *tsdn);
void        prof_postfork_child(tsdn_t *tsdn);

uint64_t tsd_prof_sample_event_wait_get(tsd_t *tsd);

extern te_base_cb_t prof_sample_te_handler;

#endif /* JEMALLOC_INTERNAL_PROF_H */
