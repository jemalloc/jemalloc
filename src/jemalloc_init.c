#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/conf.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_init.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/thread_event.h"

#ifdef JEMALLOC_THREADED_INIT
/* Used to let the initializing thread recursively allocate. */
#	define NO_INITIALIZER ((unsigned long)0)
#	define INITIALIZER pthread_self()
static pthread_t malloc_initializer = NO_INITIALIZER;
#else
#	define NO_INITIALIZER false
#	define INITIALIZER true
static bool malloc_initializer = NO_INITIALIZER;
#endif

bool
malloc_is_initializer(void) {
#ifdef JEMALLOC_THREADED_INIT
	return pthread_equal(malloc_initializer, pthread_self());
#else
	return malloc_initializer;
#endif
}

bool
malloc_initializer_is_set(void) {
	return malloc_initializer != NO_INITIALIZER;
}

void
malloc_initializer_set(void) {
	malloc_initializer = INITIALIZER;
}

/* Used to avoid initialization races. */
#ifdef _WIN32
#	if _WIN32_WINNT >= 0x0600
static malloc_mutex_t init_lock = SRWLOCK_INIT;
#	else
static malloc_mutex_t init_lock;
static bool           init_lock_initialized = false;

JEMALLOC_ATTR(constructor)
static void WINAPI
_init_init_lock(void) {
	/*
	 * If another constructor in the same binary is using mallctl to e.g.
	 * set up extent hooks, it may end up running before this one, and
	 * malloc_init_hard will crash trying to lock the uninitialized lock. So
	 * we force an initialization of the lock in malloc_init_hard as well.
	 * We don't try to care about atomicity of the accessed to the
	 * init_lock_initialized boolean, since it really only matters early in
	 * the process creation, before any separate thread normally starts
	 * doing anything.
	 */
	if (!init_lock_initialized) {
		malloc_mutex_init(&init_lock, "init", WITNESS_RANK_INIT,
		    malloc_mutex_rank_exclusive);
	}
	init_lock_initialized = true;
}

#		ifdef _MSC_VER
#			pragma section(".CRT$XCU", read)
JEMALLOC_SECTION(".CRT$XCU")
JEMALLOC_ATTR(used)
static const void(WINAPI *init_init_lock)(void) = _init_init_lock;
#		endif
#	endif
#else
static malloc_mutex_t init_lock = MALLOC_MUTEX_INITIALIZER;
#endif

malloc_init_t malloc_init_state = malloc_init_uninitialized;

/* When malloc_slow is true, set the corresponding bits for sanity check. */
enum {
	flag_opt_junk_alloc = (1U),
	flag_opt_junk_free = (1U << 1),
	flag_opt_zero = (1U << 2),
	flag_opt_utrace = (1U << 3),
	flag_opt_xmalloc = (1U << 4)
};
static uint8_t malloc_slow_flags;

static void
malloc_slow_flag_init(void) {
	/*
	 * Combine the runtime options into malloc_slow for fast path.  Called
	 * after bootstrap is complete.
	 */
	malloc_slow_flags |= (opt_junk_alloc ? flag_opt_junk_alloc : 0)
	    | (opt_junk_free ? flag_opt_junk_free : 0)
	    | (opt_zero ? flag_opt_zero : 0)
	    | (opt_utrace ? flag_opt_utrace : 0)
	    | (opt_xmalloc ? flag_opt_xmalloc : 0);

	malloc_slow = (malloc_slow_flags != 0);
}

static void stats_print_atexit(void);
static unsigned malloc_ncpus(void);
static bool malloc_cpu_count_is_deterministic(void);

static bool
malloc_init_hard_needed(void) {
	if (malloc_initialized()
	    || (malloc_is_initializer()
	        && malloc_init_state == malloc_init_recursible)) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		return false;
	}
#ifdef JEMALLOC_THREADED_INIT
	if (malloc_initializer_is_set() && !malloc_is_initializer()) {
		/* Busy-wait until the initializing thread completes. */
		spin_t spinner = SPIN_INITIALIZER;
		do {
			malloc_mutex_unlock(TSDN_NULL, &init_lock);
			spin_adaptive(&spinner);
			malloc_mutex_lock(TSDN_NULL, &init_lock);
		} while (!malloc_initialized());
		return false;
	}
#endif
	return true;
}

static bool
malloc_init_hard_a0_locked(void) {
	malloc_initializer_set();

	JEMALLOC_DIAGNOSTIC_PUSH
	JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
	sc_data_t sc_data = {0};
	JEMALLOC_DIAGNOSTIC_POP

	/*
	 * Ordering here is somewhat tricky; we need sc_boot() first, since that
	 * determines what the size classes will be, and then
	 * malloc_conf_init(), since any slab size tweaking will need to be done
	 * before sz_boot and bin_info_boot, which assume that the values they
	 * read out of sc_data_global are final.
	 */
	sc_boot(&sc_data);
	unsigned bin_shard_sizes[SC_NBINS];
	bin_shard_sizes_boot(bin_shard_sizes);
	/*
	 * prof_boot0 only initializes opt_prof_prefix.  We need to do it before
	 * we parse malloc_conf options, in case malloc_conf parsing overwrites
	 * it.
	 */
	if (config_prof) {
		prof_boot0();
	}
	char readlink_buf[PATH_MAX + 1];
	readlink_buf[0] = '\0';
	malloc_conf_init(&sc_data, bin_shard_sizes, readlink_buf);
	san_init(opt_lg_san_uaf_align);
	sz_boot(&sc_data, opt_cache_oblivious);
	bin_info_boot(&sc_data, bin_shard_sizes);

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}
	}

	if (stats_boot()) {
		return true;
	}
	if (pages_boot()) {
		return true;
	}
	if (base_boot(TSDN_NULL)) {
		return true;
	}
	/* emap_global is static, hence zeroed. */
	if (emap_init(&arena_emap_global, b0get(), /* zeroed */ true)) {
		return true;
	}
	if (extent_boot()) {
		return true;
	}
	if (ctl_boot()) {
		return true;
	}
	if (config_prof) {
		prof_boot1();
	}
	if (opt_hpa && !hpa_supported()) {
		malloc_printf(
		    "<jemalloc>: HPA not supported in the current "
		    "configuration; %s.",
		    opt_abort_conf ? "aborting" : "disabling");
		if (opt_abort_conf) {
			malloc_abort_invalid_conf();
		} else {
			opt_hpa = false;
		}
	}
	if (arena_boot(&sc_data, b0get(), opt_hpa)) {
		return true;
	}
	if (tcache_boot(TSDN_NULL, b0get())) {
		return true;
	}
	if (arenas_management_boot()) {
		return true;
	}
	hook_boot();
	experimental_thread_events_boot();
	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas_auto_set(1);
	manual_arena_base_set(narenas_auto + 1);
	memset(arenas, 0, sizeof(arena_t *) * narenas_auto);
	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * arena_choose_hard().
	 */
	if (arena_init(TSDN_NULL, 0, &arena_config_default) == NULL) {
		return true;
	}

	if (opt_hpa && !hpa_supported()) {
		malloc_printf(
		    "<jemalloc>: HPA not supported in the current "
		    "configuration; %s.",
		    opt_abort_conf ? "aborting" : "disabling");
		if (opt_abort_conf) {
			malloc_abort_invalid_conf();
		} else {
			opt_hpa = false;
		}
	}

	malloc_init_state = malloc_init_a0_initialized;

	size_t buf_len = strlen(readlink_buf);
	if (buf_len > 0) {
		void *readlink_allocated = a0malloc(buf_len + 1);
		if (readlink_allocated != NULL) {
			memcpy(readlink_allocated, readlink_buf, buf_len + 1);
			opt_malloc_conf_symlink = readlink_allocated;
		}
	}

	return false;
}

bool
malloc_init_hard_a0(void) {
	bool ret;

	malloc_mutex_lock(TSDN_NULL, &init_lock);
	ret = malloc_init_hard_a0_locked();
	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	return ret;
}

static void
stats_print_atexit(void) {
	if (config_stats) {
		tsdn_t  *tsdn;
		unsigned narenas, i;

		tsdn = tsdn_fetch();

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
			arena_t *arena = arena_get(tsdn, i, false);
			if (arena != NULL) {
				tcache_slow_t *tcache_slow;

				malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
				ql_foreach (
				    tcache_slow, &arena->tcache_ql, link) {
					tcache_stats_merge(
					    tsdn, tcache_slow->tcache, arena);
				}
				malloc_mutex_unlock(
				    tsdn, &arena->tcache_ql_mtx);
			}
		}
	}
	je_malloc_stats_print(NULL, NULL, opt_stats_print_opts);
}

static unsigned
malloc_ncpus(void) {
	long result;

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwNumberOfProcessors;
#elif defined(CPU_COUNT)
	/*
	 * glibc >= 2.6 has the CPU_COUNT macro.
	 *
	 * glibc's sysconf() uses isspace().  glibc allocates for the first time
	 * *before* setting up the isspace tables.  Therefore we need a
	 * different method to get the number of CPUs.
	 *
	 * The getaffinity approach is also preferred when only a subset of CPUs
	 * is available, to avoid using more arenas than necessary.
	 */
	{
#	if defined(__FreeBSD__) || defined(__DragonFly__)
		cpuset_t set;
#	else
		cpu_set_t set;
#	endif
#	if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
		sched_getaffinity(0, sizeof(set), &set);
#	else
		pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#	endif
		result = CPU_COUNT(&set);
	}
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

/*
 * Ensure that number of CPUs is determistinc, i.e. it is the same based on:
 * - sched_getaffinity()
 * - _SC_NPROCESSORS_ONLN
 * - _SC_NPROCESSORS_CONF
 * Since otherwise tricky things is possible with percpu arenas in use.
 */
static bool
malloc_cpu_count_is_deterministic(void) {
#ifdef _WIN32
	return true;
#else
	long cpu_onln = sysconf(_SC_NPROCESSORS_ONLN);
	long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_onln != cpu_conf) {
		return false;
	}
#	if defined(CPU_COUNT)
#		if defined(__FreeBSD__) || defined(__DragonFly__)
	cpuset_t set;
#		else
	cpu_set_t set;
#		endif /* __FreeBSD__ */
#		if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	sched_getaffinity(0, sizeof(set), &set);
#		else  /* !JEMALLOC_HAVE_SCHED_SETAFFINITY */
	pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#		endif /* JEMALLOC_HAVE_SCHED_SETAFFINITY */
	long cpu_affinity = CPU_COUNT(&set);
	if (cpu_affinity != cpu_conf) {
		return false;
	}
#	endif         /* CPU_COUNT */
	return true;
#endif
}

/* Initialize data structures which may trigger recursive allocation. */
static bool
malloc_init_hard_recursible(void) {
	malloc_init_state = malloc_init_recursible;

	ncpus = malloc_ncpus();
	if (opt_percpu_arena != percpu_arena_disabled) {
		bool cpu_count_is_deterministic =
		    malloc_cpu_count_is_deterministic();
		if (!cpu_count_is_deterministic) {
			/*
			 * If # of CPU is not deterministic, and narenas not
			 * specified, disables per cpu arena since it may not
			 * detect CPU IDs properly.
			 */
			if (opt_narenas == 0) {
				opt_percpu_arena = percpu_arena_disabled;
				malloc_write(
				    "<jemalloc>: Number of CPUs "
				    "detected is not deterministic. Per-CPU "
				    "arena disabled.\n");
				if (opt_abort_conf) {
					malloc_abort_invalid_conf();
				}
				if (opt_abort) {
					abort();
				}
			}
		}
	}

#if (defined(JEMALLOC_HAVE_PTHREAD_ATFORK) && !defined(JEMALLOC_MUTEX_INIT_CB) \
    && !defined(JEMALLOC_ZONE) && !defined(_WIN32)                             \
    && !defined(__native_client__))
	/* LinuxThreads' pthread_atfork() allocates. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork_parent,
	        jemalloc_postfork_child)
	    != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}
#endif

	if (background_thread_boot0()) {
		return true;
	}

	return false;
}

static unsigned
malloc_narenas_default(void) {
	assert(ncpus > 0);
	/*
	 * For SMP systems, create more than one arena per CPU by
	 * default.
	 */
	if (ncpus > 1) {
		fxp_t    fxp_ncpus = FXP_INIT_INT(ncpus);
		fxp_t    goal = fxp_mul(fxp_ncpus, opt_narenas_ratio);
		uint32_t int_goal = fxp_round_nearest(goal);
		if (int_goal == 0) {
			return 1;
		}
		return int_goal;
	} else {
		return 1;
	}
}

static percpu_arena_mode_t
percpu_arena_as_initialized(percpu_arena_mode_t mode) {
	assert(!malloc_initialized());
	assert(mode <= percpu_arena_disabled);

	if (mode != percpu_arena_disabled) {
		mode += percpu_arena_mode_enabled_base;
	}

	return mode;
}

static bool
malloc_init_narenas(tsdn_t *tsdn) {
	assert(ncpus > 0);

	if (opt_percpu_arena != percpu_arena_disabled) {
		if (!have_percpu_arena || malloc_getcpu() < 0) {
			opt_percpu_arena = percpu_arena_disabled;
			malloc_printf(
			    "<jemalloc>: perCPU arena getcpu() not "
			    "available. Setting narenas to %u.\n",
			    opt_narenas ? opt_narenas
			                : malloc_narenas_default());
			if (opt_abort) {
				abort();
			}
		} else {
			if (ncpus >= MALLOCX_ARENA_LIMIT) {
				malloc_printf(
				    "<jemalloc>: narenas w/ percpu"
				    "arena beyond limit (%d)\n",
				    ncpus);
				if (opt_abort) {
					abort();
				}
				return true;
			}
			/* NB: opt_percpu_arena isn't fully initialized yet. */
			if (percpu_arena_as_initialized(opt_percpu_arena)
			        == per_phycpu_arena
			    && ncpus % 2 != 0) {
				malloc_printf(
				    "<jemalloc>: invalid "
				    "configuration -- per physical CPU arena "
				    "with odd number (%u) of CPUs (no hyper "
				    "threading?).\n",
				    ncpus);
				if (opt_abort)
					abort();
			}
			unsigned n = percpu_arena_ind_limit(
			    percpu_arena_as_initialized(opt_percpu_arena));
			if (opt_narenas < n) {
				/*
				 * If narenas is specified with percpu_arena
				 * enabled, actual narenas is set as the greater
				 * of the two. percpu_arena_choose will be free
				 * to use any of the arenas based on CPU
				 * id. This is conservative (at a small cost)
				 * but ensures correctness.
				 *
				 * If for some reason the ncpus determined at
				 * boot is not the actual number (e.g. because
				 * of affinity setting from numactl), reserving
				 * narenas this way provides a workaround for
				 * percpu_arena.
				 */
				opt_narenas = n;
			}
		}
	}
	if (opt_narenas == 0) {
		opt_narenas = malloc_narenas_default();
	}
	assert(opt_narenas > 0);

	narenas_auto_set(opt_narenas);
	/*
	 * Limit the number of arenas to the indexing range of MALLOCX_ARENA().
	 */
	if (narenas_auto >= MALLOCX_ARENA_LIMIT) {
		narenas_auto_set(MALLOCX_ARENA_LIMIT - 1);
		malloc_printf("<jemalloc>: Reducing narenas to limit (%d)\n",
		    narenas_auto);
	}
	narenas_total_set(narenas_auto);
	if (arena_init_huge(tsdn, arena_get(tsdn, 0, false))) {
		narenas_total_inc();
	}
	manual_arena_base_set(narenas_total_get());

	return false;
}

static void
malloc_init_percpu(void) {
	opt_percpu_arena = percpu_arena_as_initialized(opt_percpu_arena);
}

static bool
malloc_init_hard_finish(void) {
	if (malloc_mutex_boot()) {
		return true;
	}

	malloc_init_state = malloc_init_initialized;
	malloc_slow_flag_init();

	return false;
}

static void
malloc_init_hard_cleanup(tsdn_t *tsdn, bool reentrancy_set) {
	malloc_mutex_assert_owner(tsdn, &init_lock);
	malloc_mutex_unlock(tsdn, &init_lock);
	if (reentrancy_set) {
		assert(!tsdn_null(tsdn));
		tsd_t *tsd = tsdn_tsd(tsdn);
		assert(tsd_reentrancy_level_get(tsd) > 0);
		post_reentrancy(tsd);
	}
}

bool
malloc_init_hard(void) {
	tsd_t *tsd;

	assert(TCACHE_MAXCLASS_LIMIT <= USIZE_GROW_SLOW_THRESHOLD);
	assert(SC_LOOKUP_MAXCLASS <= USIZE_GROW_SLOW_THRESHOLD);
	/*
	 * This asserts an extreme case where TINY_MAXCLASS is larger
	 * than LARGE_MINCLASS.  It could only happen if some constants
	 * are configured miserably wrong.
	 */
	assert(SC_NTINY == 0 || SC_LG_TINY_MAXCLASS <= SC_LG_LARGE_MINCLASS);

#if defined(_WIN32) && _WIN32_WINNT < 0x0600
	_init_init_lock();
#endif
	malloc_mutex_lock(TSDN_NULL, &init_lock);

#define UNLOCK_RETURN(tsdn, ret, reentrancy)                                   \
	malloc_init_hard_cleanup(tsdn, reentrancy);                            \
	return ret;

	if (!malloc_init_hard_needed()) {
		UNLOCK_RETURN(TSDN_NULL, false, false)
	}

	if (malloc_init_state != malloc_init_a0_initialized
	    && malloc_init_hard_a0_locked()) {
		UNLOCK_RETURN(TSDN_NULL, true, false)
	}

	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	/* Recursive allocation relies on functional tsd. */
	tsd = malloc_tsd_boot0();
	if (tsd == NULL) {
		return true;
	}
	if (malloc_init_hard_recursible()) {
		return true;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &init_lock);
	/* Set reentrancy level to 1 during init. */
	pre_reentrancy(tsd, NULL);
	/* Initialize narenas before prof_boot2 (for allocation). */
	if (malloc_init_narenas(tsd_tsdn(tsd))
	    || background_thread_boot1(tsd_tsdn(tsd), b0get())) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	if (opt_hpa) {
		/*
		 * We didn't initialize arena 0 hpa_shard in arena_new, because
		 * background_thread_enabled wasn't initialized yet, but we
		 * need it to set correct value for deferral_allowed.
		 */
		arena_t         *a0 = arena_get(tsd_tsdn(tsd), 0, false);
		hpa_shard_opts_t hpa_shard_opts = opt_hpa_opts;
		hpa_shard_opts.deferral_allowed = background_thread_enabled();
		if (pa_shard_enable_hpa(tsd_tsdn(tsd), &a0->pa_shard,
		        &hpa_shard_opts, &opt_hpa_sec_opts)) {
			UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
		}
	}
	if (config_prof && prof_boot2(tsd, b0get())) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}

	malloc_init_percpu();

	if (malloc_init_hard_finish()) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	post_reentrancy(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &init_lock);

	witness_assert_lockless(
	    witness_tsd_tsdn(tsd_witness_tsdp_get_unsafe(tsd)));
	malloc_tsd_boot1();
	/* Update TSD after tsd_boot1. */
	tsd = tsd_fetch();
	if (opt_background_thread) {
		assert(have_background_thread);
		/*
		 * Need to finish init & unlock first before creating background
		 * threads (pthread_create depends on malloc).  ctl_init (which
		 * sets isthreaded) needs to be called without holding any lock.
		 */
		background_thread_ctl_init(tsd_tsdn(tsd));
		if (background_thread_create(tsd, 0)) {
			return true;
		}
	}
#undef UNLOCK_RETURN
	return false;
}
