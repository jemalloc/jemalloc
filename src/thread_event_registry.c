#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/thread_event.h"
#include "jemalloc/internal/thread_event_registry.h"
#include "jemalloc/internal/thread_event_registry.h"
#include "jemalloc/internal/tcache_externs.h"
#include "jemalloc/internal/peak_event.h"
#include "jemalloc/internal/prof_externs.h"
#include "jemalloc/internal/prof_threshold.h"
#include "jemalloc/internal/stats.h"


/* Table of all the thread events.
 *  Events share interface, but internally they will know thier
 *  data layout in tsd.
 */
te_base_cb_t *te_alloc_handlers[te_alloc_count] = {
#ifdef JEMALLOC_PROF
    &prof_sample_te_handler,
#endif
    &stats_interval_te_handler,
#ifdef JEMALLOC_STATS
    &prof_threshold_te_handler,
#endif
    &tcache_gc_te_handler,
#ifdef JEMALLOC_STATS
    &peak_te_handler,
#endif
};

te_base_cb_t *te_dalloc_handlers[te_dalloc_count] = {
	&tcache_gc_te_handler,
#ifdef JEMALLOC_STATS
	&peak_te_handler,
#endif
};
