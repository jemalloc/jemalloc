#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/activity_callback.h"
#include "jemalloc/internal/prof_threshold.h"

#include "jemalloc/internal/prof_externs.h"

/*
 * Update every 128MB by default.
 */
#define PROF_THRESHOLD_LG_WAIT_DEFAULT 27

/* Logically a prof_threshold_hook_t. */
static atomic_p_t prof_threshold_hook;
size_t opt_experimental_lg_prof_threshold = PROF_THRESHOLD_LG_WAIT_DEFAULT;

void
prof_threshold_hook_set(prof_threshold_hook_t hook) {
	atomic_store_p(&prof_threshold_hook, hook, ATOMIC_RELEASE);
}

prof_threshold_hook_t
prof_threshold_hook_get(void) {
	return (prof_threshold_hook_t)atomic_load_p(&prof_threshold_hook,
	    ATOMIC_ACQUIRE);
}

/* Invoke callback for threshold reached */
static void
prof_threshold_update(tsd_t *tsd) {
	prof_threshold_hook_t prof_threshold_hook = prof_threshold_hook_get();
	if (prof_threshold_hook == NULL) {
		return;
        }
	uint64_t alloc = tsd_thread_allocated_get(tsd);
	uint64_t dalloc = tsd_thread_deallocated_get(tsd);
	peak_t *peak = tsd_peakp_get(tsd);
	pre_reentrancy(tsd, NULL);
	prof_threshold_hook(alloc, dalloc, peak->cur_max);
	post_reentrancy(tsd);
}

uint64_t
prof_threshold_new_event_wait(tsd_t *tsd) {
	return 1 << opt_experimental_lg_prof_threshold;
}

uint64_t
prof_threshold_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

void
prof_threshold_event_handler(tsd_t *tsd, uint64_t elapsed) {
	prof_threshold_update(tsd);
}
