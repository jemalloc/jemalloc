#ifndef JEMALLOC_INTERNAL_THREAD_EVENT_REGISTRY_H
#define JEMALLOC_INTERNAL_THREAD_EVENT_REGISTRY_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/tsd.h"

/* "te" is short for "thread_event" */
enum te_alloc_e {
#ifdef JEMALLOC_PROF
    te_alloc_prof_sample,
#endif
    te_alloc_stats_interval,
#ifdef JEMALLOC_STATS
    te_alloc_prof_threshold,
#endif
    te_alloc_tcache_gc,
#ifdef JEMALLOC_STATS
    te_alloc_peak,
    te_alloc_last = te_alloc_peak,
#else
    te_alloc_last = te_alloc_tcache_gc,
#endif
    te_alloc_count = te_alloc_last + 1
};
typedef enum te_alloc_e te_alloc_t;

enum te_dalloc_e {
    te_dalloc_tcache_gc,
#ifdef JEMALLOC_STATS
    te_dalloc_peak,
    te_dalloc_last = te_dalloc_peak,
#else
    te_dalloc_last = te_dalloc_tcache_gc,
#endif
    te_dalloc_count = te_dalloc_last + 1
};
typedef enum te_dalloc_e te_dalloc_t;

/* These will live in tsd */
typedef struct te_data_s te_data_t;
struct te_data_s {
	uint64_t alloc_wait[te_alloc_count];
	uint64_t dalloc_wait[te_dalloc_count];
};
#define TE_DATA_INITIALIZER { {0}, {0} }

typedef struct te_base_cb_s te_base_cb_t;
struct te_base_cb_s {
    bool (*enabled)(void);
    uint64_t (*new_event_wait)(tsd_t *tsd);
    uint64_t (*postponed_event_wait)(tsd_t *tsd);
    void (*event_handler)(tsd_t *tsd);
};

extern te_base_cb_t *te_alloc_handlers[te_alloc_count];
extern te_base_cb_t *te_dalloc_handlers[te_dalloc_count];

#endif /* JEMALLOC_INTERNAL_THREAD_EVENT_REGISTRY_H */
