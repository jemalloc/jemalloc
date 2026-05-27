#ifndef JEMALLOC_INTERNAL_CTL_ARENA_H
#define JEMALLOC_INTERNAL_CTL_ARENA_H

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/ctl_mallctl.h"

bool ctl_arenas_init(tsd_t *tsd);
ctl_arena_t *ctl_arenas_refresh(tsdn_t *tsdn);
ctl_arena_t *ctl_arenas_i(size_t i);
uint64_t ctl_arenas_epoch_get(void);
void ctl_arenas_epoch_advance(void);
unsigned ctl_narenas_get(tsdn_t *tsdn);
bool ctl_arena_i_indexable(tsdn_t *tsdn, size_t i);
bool ctl_arenas_i_verify(size_t i, unsigned narenas);
int ctl_arena_create(tsd_t *tsd, void *oldp, size_t *oldlenp,
    const arena_config_t *config);

#endif /* JEMALLOC_INTERNAL_CTL_ARENA_H */
