#define JEMALLOC_PROF_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/thread_event.h"

/*
 * This file implements the profiling "APIs" needed by other parts of jemalloc,
 * and also manages the relevant "operational" data, mainly options and mutexes;
 * the core profiling data structures are encapsulated in prof_data.c.
 */

/******************************************************************************/

#ifdef JEMALLOC_PROF_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#ifdef JEMALLOC_PROF_LIBGCC
/*
 * We have a circular dependency -- jemalloc_internal.h tells us if we should
 * use libgcc's unwinding functionality, but after we've included that, we've
 * already hooked _Unwind_Backtrace.  We'll temporarily disable hooking.
 */
#undef _Unwind_Backtrace
#include <unwind.h>
#define _Unwind_Backtrace JEMALLOC_HOOK(_Unwind_Backtrace, test_hooks_libc_hook)
#endif

/******************************************************************************/
/* Data. */

bool		opt_prof = false;
bool		opt_prof_active = true;
bool		opt_prof_thread_active_init = true;
size_t		opt_lg_prof_sample = LG_PROF_SAMPLE_DEFAULT;
ssize_t		opt_lg_prof_interval = LG_PROF_INTERVAL_DEFAULT;
bool		opt_prof_gdump = false;
bool		opt_prof_final = false;
bool		opt_prof_leak = false;
bool		opt_prof_accum = false;
char		opt_prof_prefix[PROF_DUMP_FILENAME_LEN];

/* Accessed via prof_idump_[accum/rollback](). */
static prof_accum_t	prof_idump_accumulated;

/*
 * Initialized as opt_prof_active, and accessed via
 * prof_active_[gs]et{_unlocked,}().
 */
bool			prof_active;
static malloc_mutex_t	prof_active_mtx;

/*
 * Initialized as opt_prof_thread_active_init, and accessed via
 * prof_thread_active_init_[gs]et().
 */
static bool		prof_thread_active_init;
static malloc_mutex_t	prof_thread_active_init_mtx;

/*
 * Initialized as opt_prof_gdump, and accessed via
 * prof_gdump_[gs]et{_unlocked,}().
 */
bool			prof_gdump_val;
static malloc_mutex_t	prof_gdump_mtx;

uint64_t	prof_interval = 0;

size_t		lg_prof_sample;

/*
 * Table of mutexes that are shared among gctx's.  These are leaf locks, so
 * there is no problem with using them for more than one gctx at the same time.
 * The primary motivation for this sharing though is that gctx's are ephemeral,
 * and destroying mutexes causes complications for systems that allocate when
 * creating/destroying mutexes.
 */
static malloc_mutex_t	*gctx_locks;
static atomic_u_t	cum_gctxs; /* Atomic counter. */

/*
 * Table of mutexes that are shared among tdata's.  No operations require
 * holding multiple tdata locks, so there is no problem with using them for more
 * than one tdata at the same time, even though a gctx lock may be acquired
 * while holding a tdata lock.
 */
static malloc_mutex_t	*tdata_locks;

/* Non static to enable profiling. */
malloc_mutex_t		bt2gctx_mtx;

malloc_mutex_t	tdatas_mtx;

static uint64_t		next_thr_uid;
static malloc_mutex_t	next_thr_uid_mtx;

static malloc_mutex_t	prof_dump_filename_mtx;
static uint64_t		prof_dump_seq;
static uint64_t		prof_dump_iseq;
static uint64_t		prof_dump_mseq;
static uint64_t		prof_dump_useq;

malloc_mutex_t	prof_dump_mtx;
static char	*prof_dump_prefix = NULL;

/* Do not dump any profiles until bootstrapping is complete. */
bool			prof_booted = false;

/******************************************************************************/

static bool
prof_tctx_should_destroy(tsdn_t *tsdn, prof_tctx_t *tctx) {
	malloc_mutex_assert_owner(tsdn, tctx->tdata->lock);

	if (opt_prof_accum) {
		return false;
	}
	if (tctx->cnts.curobjs != 0) {
		return false;
	}
	if (tctx->prepared) {
		return false;
	}
	return true;
}

void
prof_alloc_rollback(tsd_t *tsd, prof_tctx_t *tctx, bool updated) {
	cassert(config_prof);

	if (tsd_reentrancy_level_get(tsd) > 0) {
		assert((uintptr_t)tctx == (uintptr_t)1U);
		return;
	}

	prof_tdata_t *tdata;

	if (updated) {
		/*
		 * Compute a new sample threshold.  This isn't very important in
		 * practice, because this function is rarely executed, so the
		 * potential for sample bias is minimal except in contrived
		 * programs.
		 */
		tdata = prof_tdata_get(tsd, true);
		if (tdata != NULL) {
			prof_sample_threshold_update(tsd);
		}
	}

	if ((uintptr_t)tctx > (uintptr_t)1U) {
		malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);
		tctx->prepared = false;
		if (prof_tctx_should_destroy(tsd_tsdn(tsd), tctx)) {
			prof_tctx_destroy(tsd, tctx);
		} else {
			malloc_mutex_unlock(tsd_tsdn(tsd), tctx->tdata->lock);
		}
	}
}

void
prof_malloc_sample_object(tsdn_t *tsdn, const void *ptr, size_t usize,
    prof_tctx_t *tctx) {
	prof_tctx_set(tsdn, ptr, usize, NULL, tctx);

	/* Get the current time and set this in the extent_t. We'll read this
	 * when free() is called. */
	nstime_t t = NSTIME_ZERO_INITIALIZER;
	nstime_update(&t);
	prof_alloc_time_set(tsdn, ptr, t);

	malloc_mutex_lock(tsdn, tctx->tdata->lock);
	tctx->cnts.curobjs++;
	tctx->cnts.curbytes += usize;
	if (opt_prof_accum) {
		tctx->cnts.accumobjs++;
		tctx->cnts.accumbytes += usize;
	}
	tctx->prepared = false;
	malloc_mutex_unlock(tsdn, tctx->tdata->lock);
}

void
prof_free_sampled_object(tsd_t *tsd, const void *ptr, size_t usize,
    prof_tctx_t *tctx) {
	malloc_mutex_lock(tsd_tsdn(tsd), tctx->tdata->lock);

	assert(tctx->cnts.curobjs > 0);
	assert(tctx->cnts.curbytes >= usize);
	tctx->cnts.curobjs--;
	tctx->cnts.curbytes -= usize;

	prof_try_log(tsd, ptr, usize, tctx);

	if (prof_tctx_should_destroy(tsd_tsdn(tsd), tctx)) {
		prof_tctx_destroy(tsd, tctx);
	} else {
		malloc_mutex_unlock(tsd_tsdn(tsd), tctx->tdata->lock);
	}
}

void
bt_init(prof_bt_t *bt, void **vec) {
	cassert(config_prof);

	bt->vec = vec;
	bt->len = 0;
}

#ifdef JEMALLOC_PROF_LIBUNWIND
static void
prof_backtrace_impl(prof_bt_t *bt) {
	int nframes;

	cassert(config_prof);
	assert(bt->len == 0);
	assert(bt->vec != NULL);

	nframes = unw_backtrace(bt->vec, PROF_BT_MAX);
	if (nframes <= 0) {
		return;
	}
	bt->len = nframes;
}
#elif (defined(JEMALLOC_PROF_LIBGCC))
static _Unwind_Reason_Code
prof_unwind_init_callback(struct _Unwind_Context *context, void *arg) {
	cassert(config_prof);

	return _URC_NO_REASON;
}

static _Unwind_Reason_Code
prof_unwind_callback(struct _Unwind_Context *context, void *arg) {
	prof_unwind_data_t *data = (prof_unwind_data_t *)arg;
	void *ip;

	cassert(config_prof);

	ip = (void *)_Unwind_GetIP(context);
	if (ip == NULL) {
		return _URC_END_OF_STACK;
	}
	data->bt->vec[data->bt->len] = ip;
	data->bt->len++;
	if (data->bt->len == data->max) {
		return _URC_END_OF_STACK;
	}

	return _URC_NO_REASON;
}

static void
prof_backtrace_impl(prof_bt_t *bt) {
	prof_unwind_data_t data = {bt, PROF_BT_MAX};

	cassert(config_prof);

	_Unwind_Backtrace(prof_unwind_callback, &data);
}
#elif (defined(JEMALLOC_PROF_GCC))
static void
prof_backtrace_impl(prof_bt_t *bt) {
#define BT_FRAME(i)							\
	if ((i) < PROF_BT_MAX) {					\
		void *p;						\
		if (__builtin_frame_address(i) == 0) {			\
			return;						\
		}							\
		p = __builtin_return_address(i);			\
		if (p == NULL) {					\
			return;						\
		}							\
		bt->vec[(i)] = p;					\
		bt->len = (i) + 1;					\
	} else {							\
		return;							\
	}

	cassert(config_prof);

	BT_FRAME(0)
	BT_FRAME(1)
	BT_FRAME(2)
	BT_FRAME(3)
	BT_FRAME(4)
	BT_FRAME(5)
	BT_FRAME(6)
	BT_FRAME(7)
	BT_FRAME(8)
	BT_FRAME(9)

	BT_FRAME(10)
	BT_FRAME(11)
	BT_FRAME(12)
	BT_FRAME(13)
	BT_FRAME(14)
	BT_FRAME(15)
	BT_FRAME(16)
	BT_FRAME(17)
	BT_FRAME(18)
	BT_FRAME(19)

	BT_FRAME(20)
	BT_FRAME(21)
	BT_FRAME(22)
	BT_FRAME(23)
	BT_FRAME(24)
	BT_FRAME(25)
	BT_FRAME(26)
	BT_FRAME(27)
	BT_FRAME(28)
	BT_FRAME(29)

	BT_FRAME(30)
	BT_FRAME(31)
	BT_FRAME(32)
	BT_FRAME(33)
	BT_FRAME(34)
	BT_FRAME(35)
	BT_FRAME(36)
	BT_FRAME(37)
	BT_FRAME(38)
	BT_FRAME(39)

	BT_FRAME(40)
	BT_FRAME(41)
	BT_FRAME(42)
	BT_FRAME(43)
	BT_FRAME(44)
	BT_FRAME(45)
	BT_FRAME(46)
	BT_FRAME(47)
	BT_FRAME(48)
	BT_FRAME(49)

	BT_FRAME(50)
	BT_FRAME(51)
	BT_FRAME(52)
	BT_FRAME(53)
	BT_FRAME(54)
	BT_FRAME(55)
	BT_FRAME(56)
	BT_FRAME(57)
	BT_FRAME(58)
	BT_FRAME(59)

	BT_FRAME(60)
	BT_FRAME(61)
	BT_FRAME(62)
	BT_FRAME(63)
	BT_FRAME(64)
	BT_FRAME(65)
	BT_FRAME(66)
	BT_FRAME(67)
	BT_FRAME(68)
	BT_FRAME(69)

	BT_FRAME(70)
	BT_FRAME(71)
	BT_FRAME(72)
	BT_FRAME(73)
	BT_FRAME(74)
	BT_FRAME(75)
	BT_FRAME(76)
	BT_FRAME(77)
	BT_FRAME(78)
	BT_FRAME(79)

	BT_FRAME(80)
	BT_FRAME(81)
	BT_FRAME(82)
	BT_FRAME(83)
	BT_FRAME(84)
	BT_FRAME(85)
	BT_FRAME(86)
	BT_FRAME(87)
	BT_FRAME(88)
	BT_FRAME(89)

	BT_FRAME(90)
	BT_FRAME(91)
	BT_FRAME(92)
	BT_FRAME(93)
	BT_FRAME(94)
	BT_FRAME(95)
	BT_FRAME(96)
	BT_FRAME(97)
	BT_FRAME(98)
	BT_FRAME(99)

	BT_FRAME(100)
	BT_FRAME(101)
	BT_FRAME(102)
	BT_FRAME(103)
	BT_FRAME(104)
	BT_FRAME(105)
	BT_FRAME(106)
	BT_FRAME(107)
	BT_FRAME(108)
	BT_FRAME(109)

	BT_FRAME(110)
	BT_FRAME(111)
	BT_FRAME(112)
	BT_FRAME(113)
	BT_FRAME(114)
	BT_FRAME(115)
	BT_FRAME(116)
	BT_FRAME(117)
	BT_FRAME(118)
	BT_FRAME(119)

	BT_FRAME(120)
	BT_FRAME(121)
	BT_FRAME(122)
	BT_FRAME(123)
	BT_FRAME(124)
	BT_FRAME(125)
	BT_FRAME(126)
	BT_FRAME(127)
#undef BT_FRAME
}
#else
static void
prof_backtrace_impl(prof_bt_t *bt) {
	cassert(config_prof);
	not_reached();
}
#endif

void
prof_backtrace(tsd_t *tsd, prof_bt_t *bt) {
	cassert(config_prof);
	pre_reentrancy(tsd, NULL);
	prof_backtrace_impl(bt);
	post_reentrancy(tsd);
}

malloc_mutex_t *
prof_gctx_mutex_choose(void) {
	unsigned ngctxs = atomic_fetch_add_u(&cum_gctxs, 1, ATOMIC_RELAXED);

	return &gctx_locks[(ngctxs - 1) % PROF_NCTX_LOCKS];
}

malloc_mutex_t *
prof_tdata_mutex_choose(uint64_t thr_uid) {
	return &tdata_locks[thr_uid % PROF_NTDATA_LOCKS];
}

/*
 * The bodies of this function and prof_leakcheck() are compiled out unless heap
 * profiling is enabled, so that it is possible to compile jemalloc with
 * floating point support completely disabled.  Avoiding floating point code is
 * important on memory-constrained systems, but it also enables a workaround for
 * versions of glibc that don't properly save/restore floating point registers
 * during dynamic lazy symbol loading (which internally calls into whatever
 * malloc implementation happens to be integrated into the application).  Note
 * that some compilers (e.g.  gcc 4.8) may use floating point registers for fast
 * memory moves, so jemalloc must be compiled with such optimizations disabled
 * (e.g.
 * -mno-sse) in order for the workaround to be complete.
 */
void
prof_sample_threshold_update(tsd_t *tsd) {
#ifdef JEMALLOC_PROF
	if (!config_prof) {
		return;
	}

	if (lg_prof_sample == 0) {
		thread_prof_sample_event_update(tsd,
		    THREAD_EVENT_MIN_START_WAIT);
		return;
	}

	/*
	 * Compute sample interval as a geometrically distributed random
	 * variable with mean (2^lg_prof_sample).
	 *
	 *                      __        __
	 *                      |  log(u)  |                     1
	 * bytes_until_sample = | -------- |, where p = ---------------
	 *                      | log(1-p) |             lg_prof_sample
	 *                                              2
	 *
	 * For more information on the math, see:
	 *
	 *   Non-Uniform Random Variate Generation
	 *   Luc Devroye
	 *   Springer-Verlag, New York, 1986
	 *   pp 500
	 *   (http://luc.devroye.org/rnbookindex.html)
	 */
	uint64_t r = prng_lg_range_u64(tsd_prng_statep_get(tsd), 53);
	double u = (double)r * (1.0/9007199254740992.0L);
	uint64_t bytes_until_sample = (uint64_t)(log(u) /
	    log(1.0 - (1.0 / (double)((uint64_t)1U << lg_prof_sample))))
	    + (uint64_t)1U;
	thread_prof_sample_event_update(tsd, bytes_until_sample);
#endif
}

int
prof_getpid(void) {
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return getpid();
#endif
}

static const char *
prof_dump_prefix_get(tsdn_t* tsdn) {
	malloc_mutex_assert_owner(tsdn, &prof_dump_filename_mtx);

	return prof_dump_prefix == NULL ? opt_prof_prefix : prof_dump_prefix;
}

static bool
prof_dump_prefix_is_empty(tsdn_t *tsdn) {
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	bool ret = (prof_dump_prefix_get(tsdn)[0] == '\0');
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
	return ret;
}

#define DUMP_FILENAME_BUFSIZE	(PATH_MAX + 1)
#define VSEQ_INVALID		UINT64_C(0xffffffffffffffff)
static void
prof_dump_filename(tsd_t *tsd, char *filename, char v, uint64_t vseq) {
	cassert(config_prof);

	assert(tsd_reentrancy_level_get(tsd) == 0);
	const char *prof_prefix = prof_dump_prefix_get(tsd_tsdn(tsd));

	if (vseq != VSEQ_INVALID) {
	        /* "<prefix>.<pid>.<seq>.v<vseq>.heap" */
		malloc_snprintf(filename, DUMP_FILENAME_BUFSIZE,
		    "%s.%d.%"FMTu64".%c%"FMTu64".heap",
		    prof_prefix, prof_getpid(), prof_dump_seq, v, vseq);
	} else {
	        /* "<prefix>.<pid>.<seq>.<v>.heap" */
		malloc_snprintf(filename, DUMP_FILENAME_BUFSIZE,
		    "%s.%d.%"FMTu64".%c.heap",
		    prof_prefix, prof_getpid(), prof_dump_seq, v);
	}
	prof_dump_seq++;
}

void
prof_get_default_filename(tsdn_t *tsdn, char *filename, uint64_t ind) {
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	malloc_snprintf(filename, PROF_DUMP_FILENAME_LEN,
	    "%s.%d.%"FMTu64".json", prof_dump_prefix_get(tsdn), prof_getpid(),
	    ind);
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
}

static void
prof_fdump(void) {
	tsd_t *tsd;
	char filename[DUMP_FILENAME_BUFSIZE];

	cassert(config_prof);
	assert(opt_prof_final);

	if (!prof_booted) {
		return;
	}
	tsd = tsd_fetch();
	assert(tsd_reentrancy_level_get(tsd) == 0);
	assert(!prof_dump_prefix_is_empty(tsd_tsdn(tsd)));

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump_filename(tsd, filename, 'f', VSEQ_INVALID);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, opt_prof_leak);
}

bool
prof_accum_init(tsdn_t *tsdn) {
	cassert(config_prof);

#ifndef JEMALLOC_ATOMIC_U64
	if (malloc_mutex_init(&prof_idump_accumulated.mtx, "prof_accum",
	    WITNESS_RANK_PROF_ACCUM, malloc_mutex_rank_exclusive)) {
		return true;
	}
	prof_idump_accumulated.accumbytes = 0;
#else
	atomic_store_u64(&prof_idump_accumulated.accumbytes, 0,
	    ATOMIC_RELAXED);
#endif
	return false;
}

bool
prof_idump_accum_impl(tsdn_t *tsdn, uint64_t accumbytes) {
	cassert(config_prof);

	bool overflow;
	uint64_t a0, a1;

	/*
	 * If the application allocates fast enough (and/or if idump is slow
	 * enough), extreme overflow here (a1 >= prof_interval * 2) can cause
	 * idump trigger coalescing.  This is an intentional mechanism that
	 * avoids rate-limiting allocation.
	 */
#ifdef JEMALLOC_ATOMIC_U64
	a0 = atomic_load_u64(&prof_idump_accumulated.accumbytes,
	    ATOMIC_RELAXED);
	do {
		a1 = a0 + accumbytes;
		assert(a1 >= a0);
		overflow = (a1 >= prof_interval);
		if (overflow) {
			a1 %= prof_interval;
		}
	} while (!atomic_compare_exchange_weak_u64(
	    &prof_idump_accumulated.accumbytes, &a0, a1, ATOMIC_RELAXED,
	    ATOMIC_RELAXED));
#else
	malloc_mutex_lock(tsdn, &prof_idump_accumulated.mtx);
	a0 = prof_idump_accumulated.accumbytes;
	a1 = a0 + accumbytes;
	overflow = (a1 >= prof_interval);
	if (overflow) {
		a1 %= prof_interval;
	}
	prof_idump_accumulated.accumbytes = a1;
	malloc_mutex_unlock(tsdn, &prof_idump_accumulated.mtx);
#endif
	return overflow;
}

void
prof_idump_rollback_impl(tsdn_t *tsdn, size_t usize) {
	cassert(config_prof);

	/*
	 * Cancel out as much of the excessive accumbytes increase as possible
	 * without underflowing.  Interval-triggered dumps occur slightly more
	 * often than intended as a result of incomplete canceling.
	 */
	uint64_t a0, a1;
#ifdef JEMALLOC_ATOMIC_U64
	a0 = atomic_load_u64(&prof_idump_accumulated.accumbytes,
	    ATOMIC_RELAXED);
	do {
		a1 = (a0 >= SC_LARGE_MINCLASS - usize)
		    ? a0 - (SC_LARGE_MINCLASS - usize) : 0;
	} while (!atomic_compare_exchange_weak_u64(
	    &prof_idump_accumulated.accumbytes, &a0, a1, ATOMIC_RELAXED,
	    ATOMIC_RELAXED));
#else
	malloc_mutex_lock(tsdn, &prof_idump_accumulated.mtx);
	a0 = prof_idump_accumulated.accumbytes;
	a1 = (a0 >= SC_LARGE_MINCLASS - usize)
	    ?  a0 - (SC_LARGE_MINCLASS - usize) : 0;
	prof_idump_accumulated.accumbytes = a1;
	malloc_mutex_unlock(tsdn, &prof_idump_accumulated.mtx);
#endif
}

bool
prof_dump_prefix_set(tsdn_t *tsdn, const char *prefix) {
	cassert(config_prof);
	ctl_mtx_assert_held(tsdn);
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	if (prof_dump_prefix == NULL) {
		malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
		/* Everything is still guarded by ctl_mtx. */
		char *buffer = base_alloc(tsdn, b0get(), PROF_DUMP_FILENAME_LEN,
		    QUANTUM);
		if (buffer == NULL) {
			return true;
		}
		malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
		prof_dump_prefix = buffer;
	}
	assert(prof_dump_prefix != NULL);

	strncpy(prof_dump_prefix, prefix, PROF_DUMP_FILENAME_LEN - 1);
	prof_dump_prefix[PROF_DUMP_FILENAME_LEN - 1] = '\0';
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);

	return false;
}

void
prof_idump(tsdn_t *tsdn) {
	tsd_t *tsd;
	prof_tdata_t *tdata;

	cassert(config_prof);

	if (!prof_booted || tsdn_null(tsdn) || !prof_active_get_unlocked()) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	if (tsd_reentrancy_level_get(tsd) > 0) {
		return;
	}

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return;
	}
	if (tdata->enq) {
		tdata->enq_idump = true;
		return;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	if (prof_dump_prefix_get(tsd_tsdn(tsd))[0] == '\0') {
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		return;
	}
	char filename[PATH_MAX + 1];
	prof_dump_filename(tsd, filename, 'i', prof_dump_iseq);
	prof_dump_iseq++;
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, false);
}

bool
prof_mdump(tsd_t *tsd, const char *filename) {
	cassert(config_prof);
	assert(tsd_reentrancy_level_get(tsd) == 0);

	if (!opt_prof || !prof_booted) {
		return true;
	}
	char filename_buf[DUMP_FILENAME_BUFSIZE];
	if (filename == NULL) {
		/* No filename specified, so automatically generate one. */
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		if (prof_dump_prefix_get(tsd_tsdn(tsd))[0] == '\0') {
			malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
			return true;
		}
		prof_dump_filename(tsd, filename_buf, 'm', prof_dump_mseq);
		prof_dump_mseq++;
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		filename = filename_buf;
	}
	return prof_dump(tsd, true, filename, false);
}

void
prof_gdump(tsdn_t *tsdn) {
	tsd_t *tsd;
	prof_tdata_t *tdata;

	cassert(config_prof);

	if (!prof_booted || tsdn_null(tsdn) || !prof_active_get_unlocked()) {
		return;
	}
	tsd = tsdn_tsd(tsdn);
	if (tsd_reentrancy_level_get(tsd) > 0) {
		return;
	}

	tdata = prof_tdata_get(tsd, false);
	if (tdata == NULL) {
		return;
	}
	if (tdata->enq) {
		tdata->enq_gdump = true;
		return;
	}

	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	if (prof_dump_prefix_get(tsdn)[0] == '\0') {
		malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
		return;
	}
	char filename[DUMP_FILENAME_BUFSIZE];
	prof_dump_filename(tsd, filename, 'u', prof_dump_useq);
	prof_dump_useq++;
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, false);
}

static uint64_t
prof_thr_uid_alloc(tsdn_t *tsdn) {
	uint64_t thr_uid;

	malloc_mutex_lock(tsdn, &next_thr_uid_mtx);
	thr_uid = next_thr_uid;
	next_thr_uid++;
	malloc_mutex_unlock(tsdn, &next_thr_uid_mtx);

	return thr_uid;
}

prof_tdata_t *
prof_tdata_init(tsd_t *tsd) {
	return prof_tdata_init_impl(tsd, prof_thr_uid_alloc(tsd_tsdn(tsd)), 0,
	    NULL, prof_thread_active_init_get(tsd_tsdn(tsd)), false);
}

static char *
prof_thread_name_alloc(tsdn_t *tsdn, const char *thread_name) {
	char *ret;
	size_t size;

	if (thread_name == NULL) {
		return NULL;
	}

	size = strlen(thread_name) + 1;
	if (size == 1) {
		return "";
	}

	ret = iallocztm(tsdn, size, sz_size2index(size), false, NULL, true,
	    arena_get(TSDN_NULL, 0, true), true);
	if (ret == NULL) {
		return NULL;
	}
	memcpy(ret, thread_name, size);
	return ret;
}

prof_tdata_t *
prof_tdata_reinit(tsd_t *tsd, prof_tdata_t *tdata) {
	uint64_t thr_uid = tdata->thr_uid;
	uint64_t thr_discrim = tdata->thr_discrim + 1;
	char *thread_name = (tdata->thread_name != NULL) ?
	    prof_thread_name_alloc(tsd_tsdn(tsd), tdata->thread_name) : NULL;
	bool active = tdata->active;

	prof_tdata_detach(tsd, tdata);
	return prof_tdata_init_impl(tsd, thr_uid, thr_discrim, thread_name,
	    active, true);
}

void
prof_tdata_cleanup(tsd_t *tsd) {
	prof_tdata_t *tdata;

	if (!config_prof) {
		return;
	}

	tdata = tsd_prof_tdata_get(tsd);
	if (tdata != NULL) {
		prof_tdata_detach(tsd, tdata);
	}
}

bool
prof_active_get(tsdn_t *tsdn) {
	bool prof_active_current;

	prof_active_assert();
	malloc_mutex_lock(tsdn, &prof_active_mtx);
	prof_active_current = prof_active;
	malloc_mutex_unlock(tsdn, &prof_active_mtx);
	return prof_active_current;
}

bool
prof_active_set(tsdn_t *tsdn, bool active) {
	bool prof_active_old;

	prof_active_assert();
	malloc_mutex_lock(tsdn, &prof_active_mtx);
	prof_active_old = prof_active;
	prof_active = active;
	malloc_mutex_unlock(tsdn, &prof_active_mtx);
	prof_active_assert();
	return prof_active_old;
}

const char *
prof_thread_name_get(tsd_t *tsd) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return "";
	}
	return (tdata->thread_name != NULL ? tdata->thread_name : "");
}

int
prof_thread_name_set(tsd_t *tsd, const char *thread_name) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;
	unsigned i;
	char *s;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return EAGAIN;
	}

	/* Validate input. */
	if (thread_name == NULL) {
		return EFAULT;
	}
	for (i = 0; thread_name[i] != '\0'; i++) {
		char c = thread_name[i];
		if (!isgraph(c) && !isblank(c)) {
			return EFAULT;
		}
	}

	s = prof_thread_name_alloc(tsd_tsdn(tsd), thread_name);
	if (s == NULL) {
		return EAGAIN;
	}

	if (tdata->thread_name != NULL) {
		idalloctm(tsd_tsdn(tsd), tdata->thread_name, NULL, NULL, true,
		    true);
		tdata->thread_name = NULL;
	}
	if (strlen(s) > 0) {
		tdata->thread_name = s;
	}
	return 0;
}

bool
prof_thread_active_get(tsd_t *tsd) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return false;
	}
	return tdata->active;
}

bool
prof_thread_active_set(tsd_t *tsd, bool active) {
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t *tdata;

	tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return true;
	}
	tdata->active = active;
	return false;
}

bool
prof_thread_active_init_get(tsdn_t *tsdn) {
	bool active_init;

	malloc_mutex_lock(tsdn, &prof_thread_active_init_mtx);
	active_init = prof_thread_active_init;
	malloc_mutex_unlock(tsdn, &prof_thread_active_init_mtx);
	return active_init;
}

bool
prof_thread_active_init_set(tsdn_t *tsdn, bool active_init) {
	bool active_init_old;

	malloc_mutex_lock(tsdn, &prof_thread_active_init_mtx);
	active_init_old = prof_thread_active_init;
	prof_thread_active_init = active_init;
	malloc_mutex_unlock(tsdn, &prof_thread_active_init_mtx);
	return active_init_old;
}

bool
prof_gdump_get(tsdn_t *tsdn) {
	bool prof_gdump_current;

	malloc_mutex_lock(tsdn, &prof_gdump_mtx);
	prof_gdump_current = prof_gdump_val;
	malloc_mutex_unlock(tsdn, &prof_gdump_mtx);
	return prof_gdump_current;
}

bool
prof_gdump_set(tsdn_t *tsdn, bool gdump) {
	bool prof_gdump_old;

	malloc_mutex_lock(tsdn, &prof_gdump_mtx);
	prof_gdump_old = prof_gdump_val;
	prof_gdump_val = gdump;
	malloc_mutex_unlock(tsdn, &prof_gdump_mtx);
	return prof_gdump_old;
}

void
prof_boot0(void) {
	cassert(config_prof);

	memcpy(opt_prof_prefix, PROF_PREFIX_DEFAULT,
	    sizeof(PROF_PREFIX_DEFAULT));
}

void
prof_boot1(void) {
	cassert(config_prof);

	/*
	 * opt_prof must be in its final state before any arenas are
	 * initialized, so this function must be executed early.
	 */

	if (opt_prof_leak && !opt_prof) {
		/*
		 * Enable opt_prof, but in such a way that profiles are never
		 * automatically dumped.
		 */
		opt_prof = true;
		opt_prof_gdump = false;
	} else if (opt_prof) {
		if (opt_lg_prof_interval >= 0) {
			prof_interval = (((uint64_t)1U) <<
			    opt_lg_prof_interval);
		}
	}
}

bool
prof_boot2(tsd_t *tsd) {
	cassert(config_prof);

	if (opt_prof) {
		unsigned i;

		lg_prof_sample = opt_lg_prof_sample;

		prof_active = opt_prof_active;
		if (malloc_mutex_init(&prof_active_mtx, "prof_active",
		    WITNESS_RANK_PROF_ACTIVE, malloc_mutex_rank_exclusive)) {
			return true;
		}

		prof_gdump_val = opt_prof_gdump;
		if (malloc_mutex_init(&prof_gdump_mtx, "prof_gdump",
		    WITNESS_RANK_PROF_GDUMP, malloc_mutex_rank_exclusive)) {
			return true;
		}

		prof_thread_active_init = opt_prof_thread_active_init;
		if (malloc_mutex_init(&prof_thread_active_init_mtx,
		    "prof_thread_active_init",
		    WITNESS_RANK_PROF_THREAD_ACTIVE_INIT,
		    malloc_mutex_rank_exclusive)) {
			return true;
		}

		if (prof_data_init(tsd)) {
			return true;
		}

		if (malloc_mutex_init(&bt2gctx_mtx, "prof_bt2gctx",
		    WITNESS_RANK_PROF_BT2GCTX, malloc_mutex_rank_exclusive)) {
			return true;
		}

		if (malloc_mutex_init(&tdatas_mtx, "prof_tdatas",
		    WITNESS_RANK_PROF_TDATAS, malloc_mutex_rank_exclusive)) {
			return true;
		}

		next_thr_uid = 0;
		if (malloc_mutex_init(&next_thr_uid_mtx, "prof_next_thr_uid",
		    WITNESS_RANK_PROF_NEXT_THR_UID, malloc_mutex_rank_exclusive)) {
			return true;
		}

		if (malloc_mutex_init(&prof_dump_filename_mtx, "prof_dump_filename",
		    WITNESS_RANK_PROF_DUMP_FILENAME, malloc_mutex_rank_exclusive)) {
			return true;
		}
		if (malloc_mutex_init(&prof_dump_mtx, "prof_dump",
		    WITNESS_RANK_PROF_DUMP, malloc_mutex_rank_exclusive)) {
			return true;
		}

		if (opt_prof_final && opt_prof_prefix[0] != '\0' &&
		    atexit(prof_fdump) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}

		if (prof_log_init(tsd)) {
			return true;
		}

		gctx_locks = (malloc_mutex_t *)base_alloc(tsd_tsdn(tsd),
		    b0get(), PROF_NCTX_LOCKS * sizeof(malloc_mutex_t),
		    CACHELINE);
		if (gctx_locks == NULL) {
			return true;
		}
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			if (malloc_mutex_init(&gctx_locks[i], "prof_gctx",
			    WITNESS_RANK_PROF_GCTX,
			    malloc_mutex_rank_exclusive)) {
				return true;
			}
		}

		tdata_locks = (malloc_mutex_t *)base_alloc(tsd_tsdn(tsd),
		    b0get(), PROF_NTDATA_LOCKS * sizeof(malloc_mutex_t),
		    CACHELINE);
		if (tdata_locks == NULL) {
			return true;
		}
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			if (malloc_mutex_init(&tdata_locks[i], "prof_tdata",
			    WITNESS_RANK_PROF_TDATA,
			    malloc_mutex_rank_exclusive)) {
				return true;
			}
		}
#ifdef JEMALLOC_PROF_LIBGCC
		/*
		 * Cause the backtracing machinery to allocate its internal
		 * state before enabling profiling.
		 */
		_Unwind_Backtrace(prof_unwind_init_callback, NULL);
#endif
	}
	prof_booted = true;

	return false;
}

void
prof_prefork0(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_prefork(tsdn, &prof_dump_mtx);
		malloc_mutex_prefork(tsdn, &bt2gctx_mtx);
		malloc_mutex_prefork(tsdn, &tdatas_mtx);
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_prefork(tsdn, &tdata_locks[i]);
		}
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_prefork(tsdn, &gctx_locks[i]);
		}
	}
}

void
prof_prefork1(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		malloc_mutex_prefork(tsdn, &prof_active_mtx);
		malloc_mutex_prefork(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_prefork(tsdn, &prof_gdump_mtx);
		malloc_mutex_prefork(tsdn, &next_thr_uid_mtx);
		malloc_mutex_prefork(tsdn, &prof_thread_active_init_mtx);
	}
}

void
prof_postfork_parent(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_postfork_parent(tsdn,
		    &prof_thread_active_init_mtx);
		malloc_mutex_postfork_parent(tsdn, &next_thr_uid_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_gdump_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_active_mtx);
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_postfork_parent(tsdn, &gctx_locks[i]);
		}
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_postfork_parent(tsdn, &tdata_locks[i]);
		}
		malloc_mutex_postfork_parent(tsdn, &tdatas_mtx);
		malloc_mutex_postfork_parent(tsdn, &bt2gctx_mtx);
		malloc_mutex_postfork_parent(tsdn, &prof_dump_mtx);
	}
}

void
prof_postfork_child(tsdn_t *tsdn) {
	if (config_prof && opt_prof) {
		unsigned i;

		malloc_mutex_postfork_child(tsdn, &prof_thread_active_init_mtx);
		malloc_mutex_postfork_child(tsdn, &next_thr_uid_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_gdump_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_dump_filename_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_active_mtx);
		for (i = 0; i < PROF_NCTX_LOCKS; i++) {
			malloc_mutex_postfork_child(tsdn, &gctx_locks[i]);
		}
		for (i = 0; i < PROF_NTDATA_LOCKS; i++) {
			malloc_mutex_postfork_child(tsdn, &tdata_locks[i]);
		}
		malloc_mutex_postfork_child(tsdn, &tdatas_mtx);
		malloc_mutex_postfork_child(tsdn, &bt2gctx_mtx);
		malloc_mutex_postfork_child(tsdn, &prof_dump_mtx);
	}
}

/******************************************************************************/
