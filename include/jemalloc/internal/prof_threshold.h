#ifndef JEMALLOC_INTERNAL_THRESHOLD_EVENT_H
#define JEMALLOC_INTERNAL_THRESHOLD_EVENT_H

#include "jemalloc/internal/tsd_types.h"

/* The activity-triggered hooks. */
uint64_t prof_threshold_new_event_wait(tsd_t *tsd);
uint64_t prof_threshold_postponed_event_wait(tsd_t *tsd);
void prof_threshold_event_handler(tsd_t *tsd, uint64_t elapsed);

#endif /* JEMALLOC_INTERNAL_THRESHOLD_EVENT_H */
