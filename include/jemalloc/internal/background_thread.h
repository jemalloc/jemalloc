#ifndef JEMALLOC_INTERNAL_BACKGROUND_THREAD_H
#define JEMALLOC_INTERNAL_BACKGROUND_THREAD_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/os.h"

#if defined(JEMALLOC_BACKGROUND_THREAD) || defined(JEMALLOC_LAZY_LOCK)
#	define JEMALLOC_PTHREAD_CREATE_WRAPPER
#endif

#define BACKGROUND_THREAD_INDEFINITE_SLEEP UINT64_MAX
#define MAX_BACKGROUND_THREAD_LIMIT MALLOCX_ARENA_LIMIT
#define DEFAULT_NUM_BACKGROUND_THREAD 4

typedef enum {
	background_thread_stopped,
	background_thread_started,
	/* Thread waits on its condition variable when paused (arena_reset). */
	background_thread_paused,
} background_thread_state_t;

struct background_thread_info_s {
#ifdef JEMALLOC_BACKGROUND_THREAD
	/* Background thread is pthread specific. */
	pthread_t thread;
	os_cond_t cond;
#endif
	malloc_mutex_t            mtx;
	background_thread_state_t state;
	/* When true, it means no wakeup scheduled. */
	atomic_b_t indefinite_sleep;
	/* Next scheduled wakeup time (absolute time in ns). */
	nstime_t next_wakeup;
	/*
	 *  Since the last background thread run, newly added number of pages
	 *  that need to be purged by the next wakeup.  This is adjusted on
	 *  epoch advance, and is used to determine whether we should signal the
	 *  background thread to wake up earlier.
	 */
	size_t npages_to_purge_new;
	/* Stats: total number of runs since started. */
	uint64_t tot_n_runs;
	/* Stats: total sleep time since started. */
	nstime_t tot_sleep_time;
};
typedef struct background_thread_info_s background_thread_info_t;

struct background_thread_stats_s {
	size_t            num_threads;
	uint64_t          num_runs;
	nstime_t          run_interval;
	mutex_prof_data_t max_counter_per_bg_thd;
};
typedef struct background_thread_stats_s background_thread_stats_t;

extern bool                      opt_background_thread;
extern size_t                    opt_max_background_threads;
extern malloc_mutex_t            background_thread_lock;
extern atomic_b_t                background_thread_enabled_state;
extern size_t                    n_background_threads;
extern size_t                    max_background_threads;
extern background_thread_info_t *background_thread_info;

bool background_thread_create(tsd_t *tsd, unsigned arena_ind);
bool background_threads_enable(tsd_t *tsd);
bool background_threads_disable(tsd_t *tsd);
bool background_thread_is_started(background_thread_info_t *info);
void background_thread_wakeup_early(
    background_thread_info_t *info, nstime_t *remaining_sleep);
void background_thread_prefork0(tsdn_t *tsdn);
void background_thread_prefork1(tsdn_t *tsdn);
void background_thread_postfork_parent(tsdn_t *tsdn);
void background_thread_postfork_child(tsdn_t *tsdn);
bool background_thread_stats_read(
    tsdn_t *tsdn, background_thread_stats_t *stats);
void background_thread_ctl_init(tsdn_t *tsdn);

/*
 * Arena-reset lifecycle bracket (owned by the bg-thread module so the
 * started<->paused state machine is mutated only inside the module).  begin()
 * acquires the global background_thread_lock and RETURNS WITH IT HELD;
 * finish() releases it.  The lock intentionally spans the whole arena-reset
 * body across the two calls.  Pass arena_ind (not an arena_t *): the destroy
 * path frees the arena before calling finish().
 */
void background_thread_arena_reset_begin(tsd_t *tsd, unsigned arena_ind);
void background_thread_arena_reset_finish(tsd_t *tsd, unsigned arena_ind);

/*
 * Serialize a rare admin operation against the background thread by holding the
 * per-arena info mutex (have_background_thread-gated).  Used by
 * arena_set_extent_hooks to fence pa_shard_disable_hpa.
 */
void background_thread_serialize_lock(tsd_t *tsd, unsigned arena_ind);
void background_thread_serialize_unlock(tsd_t *tsd, unsigned arena_ind);

#ifdef JEMALLOC_PTHREAD_CREATE_WRAPPER
extern int pthread_create_wrapper(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);
#endif
bool background_thread_boot0(void);
bool background_thread_boot1(tsdn_t *tsdn, base_t *base);

#endif /* JEMALLOC_INTERNAL_BACKGROUND_THREAD_H */
