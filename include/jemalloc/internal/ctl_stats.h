#ifndef JEMALLOC_INTERNAL_CTL_STATS_H
#define JEMALLOC_INTERNAL_CTL_STATS_H

#include "jemalloc/internal/ctl_mallctl.h"

bool ctl_stats_init(tsdn_t *tsdn);
void ctl_stats_refresh(tsdn_t *tsdn, ctl_arena_t *ctl_sarena);

#endif /* JEMALLOC_INTERNAL_CTL_STATS_H */
