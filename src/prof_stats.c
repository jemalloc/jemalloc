#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/prof_inlines.h"
#include "jemalloc/internal/prof_stats.h"

bool                opt_prof_stats = false;
malloc_mutex_t      prof_stats_mtx;
static prof_stats_t prof_stats_live[PROF_SC_NSIZES];
static prof_stats_t prof_stats_accum[PROF_SC_NSIZES];

#if defined(JEMALLOC_PROF) && defined(JEMALLOC_STATS)
JEMALLOC_ALIGNED(CACHELINE)
static struct {
	atomic_zu_t nmissed;
	atomic_zu_t bytes_missed;
	char padding[CACHELINE - 2 * sizeof(atomic_zu_t)];
} prof_sample_stats = {ATOMIC_INIT(0), ATOMIC_INIT(0)};
#endif

size_t
prof_sample_nmissed_get(void) {
#if defined(JEMALLOC_PROF) && defined(JEMALLOC_STATS)
	return atomic_load_zu(&prof_sample_stats.nmissed, ATOMIC_RELAXED);
#else
	return 0;
#endif
}

size_t
prof_sample_bytes_missed_get(void) {
#if defined(JEMALLOC_PROF) && defined(JEMALLOC_STATS)
	return atomic_load_zu(&prof_sample_stats.bytes_missed, ATOMIC_RELAXED);
#else
	return 0;
#endif
}

/*
 * Count an unsampled allocation from a reentrant thread if an active sampler
 * would have selected it.  Thread events for this allocation run after the
 * commit, so these are the counters prof_sample_lookahead()'s test applies to.
 */
JEMALLOC_COLD void
prof_sample_missed_check(tsd_t *tsd, size_t usize) {
#if defined(JEMALLOC_PROF) && defined(JEMALLOC_STATS)
	if (!prof_active_get_unlocked()) {
		return;
	}
	/* The subtraction is intentionally susceptible to underflow. */
	uint64_t accumbytes = tsd_thread_allocated_get(tsd) + usize
	    - tsd_thread_allocated_last_event_get(tsd);
	if (accumbytes >= tsd_prof_sample_event_wait_get(tsd)) {
		atomic_fetch_add_zu(
		    &prof_sample_stats.nmissed, 1, ATOMIC_RELAXED);
		atomic_fetch_add_zu(
		    &prof_sample_stats.bytes_missed, usize, ATOMIC_RELAXED);
	}
#endif
}

static void
prof_stats_enter(tsd_t *tsd, szind_t ind) {
	assert(opt_prof && opt_prof_stats);
	assert(ind < SC_NSIZES);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_stats_mtx);
}

static void
prof_stats_leave(tsd_t *tsd) {
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_stats_mtx);
}

void
prof_stats_inc(tsd_t *tsd, szind_t ind, size_t size) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	prof_stats_live[ind].req_sum += size;
	prof_stats_live[ind].count++;
	prof_stats_accum[ind].req_sum += size;
	prof_stats_accum[ind].count++;
	prof_stats_leave(tsd);
}

void
prof_stats_dec(tsd_t *tsd, szind_t ind, size_t size) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	prof_stats_live[ind].req_sum -= size;
	prof_stats_live[ind].count--;
	prof_stats_leave(tsd);
}

void
prof_stats_get_live(tsd_t *tsd, szind_t ind, prof_stats_t *stats) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	memcpy(stats, &prof_stats_live[ind], sizeof(prof_stats_t));
	prof_stats_leave(tsd);
}

void
prof_stats_get_accum(tsd_t *tsd, szind_t ind, prof_stats_t *stats) {
	cassert(config_prof);
	prof_stats_enter(tsd, ind);
	memcpy(stats, &prof_stats_accum[ind], sizeof(prof_stats_t));
	prof_stats_leave(tsd);
}
