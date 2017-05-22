#define JEMALLOC_BACKGROUND_THREAD_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

/******************************************************************************/
/* Data. */

/* This option should be opt-in only. */
#define BACKGROUND_THREAD_DEFAULT false
/* Read-only after initialization. */
bool opt_background_thread = BACKGROUND_THREAD_DEFAULT;

/* Used for thread creation, termination and stats. */
malloc_mutex_t background_thread_lock;
/* Indicates global state.  Atomic because decay reads this w/o locking. */
atomic_b_t background_thread_enabled_state;
size_t n_background_threads;
/* Thread info per-index. */
background_thread_info_t *background_thread_info;

/******************************************************************************/

#ifndef JEMALLOC_BACKGROUND_THREAD
#define NOT_REACHED { not_reached(); }
bool background_thread_create(tsd_t *tsd, unsigned arena_ind) NOT_REACHED
bool background_threads_init(tsd_t *tsd) NOT_REACHED
bool background_threads_enable(tsd_t *tsd) NOT_REACHED
bool background_threads_disable(tsd_t *tsd) NOT_REACHED
bool background_threads_disable_single(tsd_t *tsd,
    background_thread_info_t *info) NOT_REACHED
void background_thread_interval_check(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, size_t npages_new) NOT_REACHED
void background_thread_prefork0(tsdn_t *tsdn) NOT_REACHED
void background_thread_prefork1(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_parent(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_child(tsdn_t *tsdn) NOT_REACHED
bool background_thread_stats_read(tsdn_t *tsdn,
    background_thread_stats_t *stats) NOT_REACHED
#undef NOT_REACHED
#else

static void
background_thread_info_reinit(tsdn_t *tsdn, background_thread_info_t *info) {
	background_thread_wakeup_time_set(tsdn, info, 0);
	info->npages_to_purge_new = 0;
	if (config_stats) {
		info->tot_n_runs = 0;
		nstime_init(&info->tot_sleep_time, 0);
	}
}

bool
background_threads_init(tsd_t *tsd) {
	assert(have_background_thread);
	assert(narenas_total_get() > 0);

	background_thread_enabled_set(tsd_tsdn(tsd), opt_background_thread);
	if (malloc_mutex_init(&background_thread_lock,
	    "background_thread_global",
	    WITNESS_RANK_BACKGROUND_THREAD_GLOBAL,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	background_thread_info = (background_thread_info_t *)base_alloc(
	    tsd_tsdn(tsd), b0get(), ncpus * sizeof(background_thread_info_t),
	    CACHELINE);
	if (background_thread_info == NULL) {
		return true;
	}

	for (unsigned i = 0; i < ncpus; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		if (malloc_mutex_init(&info->mtx, "background_thread",
		    WITNESS_RANK_BACKGROUND_THREAD,
		    malloc_mutex_rank_exclusive)) {
			return true;
		}
		if (pthread_cond_init(&info->cond, NULL)) {
			return true;
		}
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		info->started = false;
		background_thread_info_reinit(tsd_tsdn(tsd), info);
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	}

	return false;
}

static inline bool
set_current_thread_affinity(UNUSED int cpu) {
#if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	int ret = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

	return (ret != 0);
#else
	return false;
#endif
}

/* Threshold for determining when to wake up the background thread. */
#define BACKGROUND_THREAD_NPAGES_THRESHOLD UINT64_C(1024)
#define BILLION UINT64_C(1000000000)
/* Minimal sleep interval 100 ms. */
#define BACKGROUND_THREAD_MIN_INTERVAL_NS (BILLION / 10)

static inline size_t
decay_npurge_after_interval(arena_decay_t *decay, size_t interval) {
	size_t i;
	uint64_t sum = 0;
	for (i = 0; i < interval; i++) {
		sum += decay->backlog[i] * h_steps[i];
	}
	for (; i < SMOOTHSTEP_NSTEPS; i++) {
		sum += decay->backlog[i] * (h_steps[i] - h_steps[i - interval]);
	}

	return (size_t)(sum >> SMOOTHSTEP_BFP);
}

static uint64_t
arena_decay_compute_purge_interval_impl(tsdn_t *tsdn, arena_decay_t *decay,
    extents_t *extents) {
	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		/* Use minimal interval if decay is contended. */
		return BACKGROUND_THREAD_MIN_INTERVAL_NS;
	}

	uint64_t interval;
	ssize_t decay_time = atomic_load_zd(&decay->time_ms, ATOMIC_RELAXED);
	if (decay_time <= 0) {
		/* Purging is eagerly done or disabled currently. */
		interval = BACKGROUND_THREAD_INDEFINITE_SLEEP;
		goto label_done;
	}

	uint64_t decay_interval_ns = nstime_ns(&decay->interval);
	assert(decay_interval_ns > 0);
	size_t npages = extents_npages_get(extents);
	if (npages == 0) {
		unsigned i;
		for (i = 0; i < SMOOTHSTEP_NSTEPS; i++) {
			if (decay->backlog[i] > 0) {
				break;
			}
		}
		if (i == SMOOTHSTEP_NSTEPS) {
			/* No dirty pages recorded.  Sleep indefinitely. */
			interval = BACKGROUND_THREAD_INDEFINITE_SLEEP;
			goto label_done;
		}
	}
	if (npages <= BACKGROUND_THREAD_NPAGES_THRESHOLD) {
		/* Use max interval. */
		interval = decay_interval_ns * SMOOTHSTEP_NSTEPS;
		goto label_done;
	}

	size_t lb = BACKGROUND_THREAD_MIN_INTERVAL_NS / decay_interval_ns;
	size_t ub = SMOOTHSTEP_NSTEPS;
	/* Minimal 2 intervals to ensure reaching next epoch deadline. */
	lb = (lb < 2) ? 2 : lb;
	if ((decay_interval_ns * ub <= BACKGROUND_THREAD_MIN_INTERVAL_NS) ||
	    (lb + 2 > ub)) {
		interval = BACKGROUND_THREAD_MIN_INTERVAL_NS;
		goto label_done;
	}

	assert(lb + 2 <= ub);
	size_t npurge_lb, npurge_ub;
	npurge_lb = decay_npurge_after_interval(decay, lb);
	if (npurge_lb > BACKGROUND_THREAD_NPAGES_THRESHOLD) {
		interval = decay_interval_ns * lb;
		goto label_done;
	}
	npurge_ub = decay_npurge_after_interval(decay, ub);
	if (npurge_ub < BACKGROUND_THREAD_NPAGES_THRESHOLD) {
		interval = decay_interval_ns * ub;
		goto label_done;
	}

	unsigned n_search = 0;
	size_t target, npurge;
	while ((npurge_lb + BACKGROUND_THREAD_NPAGES_THRESHOLD < npurge_ub)
	    && (lb + 2 < ub)) {
		target = (lb + ub) / 2;
		npurge = decay_npurge_after_interval(decay, target);
		if (npurge > BACKGROUND_THREAD_NPAGES_THRESHOLD) {
			ub = target;
			npurge_ub = npurge;
		} else {
			lb = target;
			npurge_lb = npurge;
		}
		assert(n_search++ < lg_floor(SMOOTHSTEP_NSTEPS) + 1);
	}
	interval = decay_interval_ns * (ub + lb) / 2;
label_done:
	interval = (interval < BACKGROUND_THREAD_MIN_INTERVAL_NS) ?
	    BACKGROUND_THREAD_MIN_INTERVAL_NS : interval;
	malloc_mutex_unlock(tsdn, &decay->mtx);

	return interval;
}

/* Compute purge interval for background threads. */
static uint64_t
arena_decay_compute_purge_interval(tsdn_t *tsdn, arena_t *arena) {
	uint64_t i1, i2;
	i1 = arena_decay_compute_purge_interval_impl(tsdn, &arena->decay_dirty,
	    &arena->extents_dirty);
	if (i1 == BACKGROUND_THREAD_MIN_INTERVAL_NS) {
		return i1;
	}
	i2 = arena_decay_compute_purge_interval_impl(tsdn, &arena->decay_muzzy,
	    &arena->extents_muzzy);

	return i1 < i2 ? i1 : i2;
}

static inline uint64_t
background_work_once(tsdn_t *tsdn, unsigned ind) {
	arena_t *arena;
	unsigned i, narenas;
	uint64_t min_interval;

	min_interval = BACKGROUND_THREAD_INDEFINITE_SLEEP;
	narenas = narenas_total_get();
	for (i = ind; i < narenas; i += ncpus) {
		arena = arena_get(tsdn, i, false);
		if (!arena) {
			continue;
		}

		arena_decay(tsdn, arena, true, false);
		uint64_t interval = arena_decay_compute_purge_interval(tsdn,
		    arena);
		if (interval == BACKGROUND_THREAD_MIN_INTERVAL_NS) {
			return interval;
		}

		assert(interval > BACKGROUND_THREAD_MIN_INTERVAL_NS);
		if (min_interval > interval) {
			min_interval = interval;
		}
	}

	return min_interval;
}

static void
background_work(tsdn_t *tsdn, unsigned ind) {
	int ret;
	background_thread_info_t *info = &background_thread_info[ind];

	malloc_mutex_lock(tsdn, &info->mtx);
	background_thread_wakeup_time_set(tsdn, info,
	    BACKGROUND_THREAD_INDEFINITE_SLEEP);
	while (info->started) {
		uint64_t interval = background_work_once(tsdn, ind);
		if (config_stats) {
			info->tot_n_runs++;
		}
		info->npages_to_purge_new = 0;

		struct timeval tv;
		/* Specific clock required by timedwait. */
		gettimeofday(&tv, NULL);
		nstime_t before_sleep;
		nstime_init2(&before_sleep, tv.tv_sec, tv.tv_usec * 1000);

		if (interval == BACKGROUND_THREAD_INDEFINITE_SLEEP) {
			assert(background_thread_indefinite_sleep(info));
			ret = pthread_cond_wait(&info->cond, &info->mtx.lock);
			assert(ret == 0);
		} else {
			assert(interval >= BACKGROUND_THREAD_MIN_INTERVAL_NS &&
			    interval <= BACKGROUND_THREAD_INDEFINITE_SLEEP);
			/* We need malloc clock (can be different from tv). */
			nstime_t next_wakeup;
			nstime_init(&next_wakeup, 0);
			nstime_update(&next_wakeup);
			nstime_iadd(&next_wakeup, interval);
			assert(nstime_ns(&next_wakeup) <
			    BACKGROUND_THREAD_INDEFINITE_SLEEP);
			background_thread_wakeup_time_set(tsdn, info,
			    nstime_ns(&next_wakeup));

			nstime_t ts_wakeup;
			nstime_copy(&ts_wakeup, &before_sleep);
			nstime_iadd(&ts_wakeup, interval);
			struct timespec ts;
			ts.tv_sec = (size_t)nstime_sec(&ts_wakeup);
			ts.tv_nsec = (size_t)nstime_nsec(&ts_wakeup);

			assert(!background_thread_indefinite_sleep(info));
			ret = pthread_cond_timedwait(&info->cond,
			    &info->mtx.lock, &ts);
			assert(ret == ETIMEDOUT || ret == 0);
			background_thread_wakeup_time_set(tsdn, info,
			    BACKGROUND_THREAD_INDEFINITE_SLEEP);
		}

		if (config_stats) {
			gettimeofday(&tv, NULL);
			nstime_t after_sleep;
			nstime_init2(&after_sleep, tv.tv_sec, tv.tv_usec * 1000);
			if (nstime_compare(&after_sleep, &before_sleep) > 0) {
				nstime_subtract(&after_sleep, &before_sleep);
				nstime_add(&info->tot_sleep_time, &after_sleep);
			}
		}
	}
	background_thread_wakeup_time_set(tsdn, info, 0);
	malloc_mutex_unlock(tsdn, &info->mtx);
}

static void *
background_thread_entry(void *ind_arg) {
	unsigned thread_ind = (unsigned)(uintptr_t)ind_arg;
	assert(thread_ind < narenas_total_get() && thread_ind < ncpus);

	if (opt_percpu_arena != percpu_arena_disabled) {
		set_current_thread_affinity((int)thread_ind);
	}
	/*
	 * Start periodic background work.  We avoid fetching tsd to keep the
	 * background thread "outside", since there may be side effects, for
	 * example triggering new arena creation (which in turn triggers
	 * background thread creation).
	 */
	background_work(TSDN_NULL, thread_ind);
	assert(pthread_equal(pthread_self(),
	    background_thread_info[thread_ind].thread));

	return NULL;
}

/* Create a new background thread if needed. */
bool
background_thread_create(tsd_t *tsd, unsigned arena_ind) {
	assert(have_background_thread);
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);

	/* We create at most NCPUs threads. */
	size_t thread_ind = arena_ind % ncpus;
	background_thread_info_t *info = &background_thread_info[thread_ind];

	bool need_new_thread;
	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	need_new_thread = background_thread_enabled() && !info->started;
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	if (!need_new_thread) {
		return false;
	}

	pre_reentrancy(tsd);
	int err;
	load_pthread_create_fptr();
	if ((err = pthread_create(&info->thread, NULL,
	    background_thread_entry, (void *)thread_ind)) != 0) {
		malloc_printf("<jemalloc>: arena %u background thread creation "
		    "failed (%d).\n", arena_ind, err);
	}
	post_reentrancy(tsd);

	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	assert(info->started == false);
	if (err == 0) {
		info->started = true;
		background_thread_info_reinit(tsd_tsdn(tsd), info);
		n_background_threads++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);

	return (err != 0);
}

bool
background_threads_enable(tsd_t *tsd) {
	assert(n_background_threads == 0);
	assert(background_thread_enabled());
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);

	VARIABLE_ARRAY(bool, created, ncpus);
	unsigned i, ncreated;
	for (i = 0; i < ncpus; i++) {
		created[i] = false;
	}
	ncreated = 0;

	unsigned n = narenas_total_get();
	for (i = 0; i < n; i++) {
		if (created[i % ncpus] ||
		    arena_get(tsd_tsdn(tsd), i, false) == NULL) {
			continue;
		}
		if (background_thread_create(tsd, i)) {
			return true;
		}
		created[i % ncpus] = true;
		if (++ncreated == ncpus) {
			break;
		}
	}

	return false;
}

bool
background_threads_disable_single(tsd_t *tsd, background_thread_info_t *info) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);
	pre_reentrancy(tsd);

	bool has_thread;
	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	if (info->started) {
		has_thread = true;
		info->started = false;
		pthread_cond_signal(&info->cond);
	} else {
		has_thread = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);

	if (!has_thread) {
		post_reentrancy(tsd);
		return false;
	}
	void *ret;
	if (pthread_join(info->thread, &ret)) {
		post_reentrancy(tsd);
		return true;
	}
	assert(ret == NULL);
	n_background_threads--;
	post_reentrancy(tsd);

	return false;
}

bool
background_threads_disable(tsd_t *tsd) {
	assert(!background_thread_enabled());
	for (unsigned i = 0; i < ncpus; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		if (background_threads_disable_single(tsd, info)) {
			return true;
		}
	}
	assert(n_background_threads == 0);

	return false;
}

/* Check if we need to signal the background thread early. */
void
background_thread_interval_check(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, size_t npages_new) {
	background_thread_info_t *info = arena_background_thread_info_get(
	    arena);

	if (malloc_mutex_trylock(tsdn, &info->mtx)) {
		/*
		 * Background thread may hold the mutex for a long period of
		 * time.  We'd like to avoid the variance on application
		 * threads.  So keep this non-blocking, and leave the work to a
		 * future epoch.
		 */
		return;
	}

	if (!info->started) {
		goto label_done;
	}
	assert(background_thread_enabled());
	if (malloc_mutex_trylock(tsdn, &decay->mtx)) {
		goto label_done;
	}

	ssize_t decay_time = atomic_load_zd(&decay->time_ms, ATOMIC_RELAXED);
	if (decay_time <= 0) {
		/* Purging is eagerly done or disabled currently. */
		goto label_done_unlock2;
	}
	uint64_t decay_interval_ns = nstime_ns(&decay->interval);
	assert(decay_interval_ns > 0);

	nstime_t diff;
	nstime_init(&diff, background_thread_wakeup_time_get(info));
	if (nstime_compare(&diff, &decay->epoch) <= 0) {
		goto label_done_unlock2;
	}
	nstime_subtract(&diff, &decay->epoch);
	if (nstime_ns(&diff) < BACKGROUND_THREAD_MIN_INTERVAL_NS) {
		goto label_done_unlock2;
	}

	if (npages_new > 0) {
		size_t n_epoch = (size_t)(nstime_ns(&diff) / decay_interval_ns);
		/*
		 * Compute how many new pages we would need to purge by the next
		 * wakeup, which is used to determine if we should signal the
		 * background thread.
		 */
		uint64_t npurge_new;
		if (n_epoch >= SMOOTHSTEP_NSTEPS) {
			npurge_new = npages_new;
		} else {
			uint64_t h_steps_max = h_steps[SMOOTHSTEP_NSTEPS - 1];
			assert(h_steps_max >=
			    h_steps[SMOOTHSTEP_NSTEPS - 1 - n_epoch]);
			npurge_new = npages_new * (h_steps_max -
			    h_steps[SMOOTHSTEP_NSTEPS - 1 - n_epoch]);
			npurge_new >>= SMOOTHSTEP_BFP;
		}
		info->npages_to_purge_new += npurge_new;
	}

	bool should_signal;
	if (info->npages_to_purge_new > BACKGROUND_THREAD_NPAGES_THRESHOLD) {
		should_signal = true;
	} else if (unlikely(background_thread_indefinite_sleep(info)) &&
	    (extents_npages_get(&arena->extents_dirty) > 0 ||
	    extents_npages_get(&arena->extents_muzzy) > 0 ||
	    info->npages_to_purge_new > 0)) {
		should_signal = true;
	} else {
		should_signal = false;
	}

	if (should_signal) {
		info->npages_to_purge_new = 0;
		pthread_cond_signal(&info->cond);
	}
label_done_unlock2:
	malloc_mutex_unlock(tsdn, &decay->mtx);
label_done:
	malloc_mutex_unlock(tsdn, &info->mtx);
}

void
background_thread_prefork0(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &background_thread_lock);
	if (background_thread_enabled()) {
		background_thread_enabled_set(tsdn, false);
		background_threads_disable(tsdn_tsd(tsdn));
		/* Enable again to re-create threads after fork. */
		background_thread_enabled_set(tsdn, true);
	}
	assert(n_background_threads == 0);
}

void
background_thread_prefork1(tsdn_t *tsdn) {
	for (unsigned i = 0; i < ncpus; i++) {
		malloc_mutex_prefork(tsdn, &background_thread_info[i].mtx);
	}
}

static void
background_thread_postfork_init(tsdn_t *tsdn) {
	if (background_thread_enabled()) {
		background_threads_enable(tsdn_tsd(tsdn));
	}
}

void
background_thread_postfork_parent(tsdn_t *tsdn) {
	for (unsigned i = 0; i < ncpus; i++) {
		malloc_mutex_postfork_parent(tsdn,
		    &background_thread_info[i].mtx);
	}
	background_thread_postfork_init(tsdn);
	malloc_mutex_postfork_parent(tsdn, &background_thread_lock);
}

void
background_thread_postfork_child(tsdn_t *tsdn) {
	for (unsigned i = 0; i < ncpus; i++) {
		malloc_mutex_postfork_child(tsdn,
		    &background_thread_info[i].mtx);
	}
	malloc_mutex_postfork_child(tsdn, &background_thread_lock);

	malloc_mutex_lock(tsdn, &background_thread_lock);
	background_thread_postfork_init(tsdn);
	malloc_mutex_unlock(tsdn, &background_thread_lock);
}

bool
background_thread_stats_read(tsdn_t *tsdn, background_thread_stats_t *stats) {
	assert(config_stats);
	malloc_mutex_lock(tsdn, &background_thread_lock);
	if (!background_thread_enabled()) {
		malloc_mutex_unlock(tsdn, &background_thread_lock);
		return true;
	}

	stats->num_threads = n_background_threads;
	uint64_t num_runs = 0;
	nstime_init(&stats->run_interval, 0);
	for (unsigned i = 0; i < ncpus; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		malloc_mutex_lock(tsdn, &info->mtx);
		if (info->started) {
			num_runs += info->tot_n_runs;
			nstime_add(&stats->run_interval, &info->tot_sleep_time);
		}
		malloc_mutex_unlock(tsdn, &info->mtx);
	}
	stats->num_runs = num_runs;
	if (num_runs > 0) {
		nstime_idivide(&stats->run_interval, num_runs);
	}
	malloc_mutex_unlock(tsdn, &background_thread_lock);

	return false;
}

#undef BACKGROUND_THREAD_NPAGES_THRESHOLD
#undef BILLION
#undef BACKGROUND_THREAD_MIN_INTERVAL_NS

#endif /* defined(JEMALLOC_BACKGROUND_THREAD) */

#if defined(JEMALLOC_BACKGROUND_THREAD) || defined(JEMALLOC_LAZY_LOCK)
#include <dlfcn.h>

int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

void *
load_pthread_create_fptr(void) {
	if (pthread_create_fptr) {
		return pthread_create_fptr;
	}

	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
	if (pthread_create_fptr == NULL) {
		malloc_write("<jemalloc>: Error in dlsym(RTLD_NEXT, "
		    "\"pthread_create\")\n");
		abort();
	}

	return pthread_create_fptr;
}

#endif
