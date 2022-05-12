#ifndef JEMALLOC_INTERNAL_CCACHE_H
#define JEMALLOC_INTERNAL_CCACHE_H

#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/pages.h"

#include "jemalloc/internal/arena_types.h"
#include "jemalloc/internal/tsd_types.h"
#include "jemalloc/internal/base.h"

extern bool opt_ccache;
extern size_t opt_ccache_max;

extern szind_t ccache_minind;
extern szind_t ccache_maxind;
extern size_t ccache_maxclass;

bool ccache_init(tsdn_t *tsdn, base_t *base);
void* ccache_alloc(tsd_t *tsd, arena_t *arena,
    size_t size, szind_t ind, bool zero, bool small);
bool ccache_free(tsd_t *tsd, void *ptr, szind_t ind, bool small);

/* Stats */
uint32_t ccache_nflushes_get();
uint32_t ccache_nfills_get();

/* Functions operating on TLS */
bool tsd_ccache_init(tsd_t *tsd);
bool ccache_cleanup(tsd_t *tsd);
void ccache_merge_tstats(tsdn_t *tsdn);

/* Non thread-safe functions, use only without contention, e.g. in tests */
void ccache_full_flush_unsafe();
bool ccache_is_empty_unsafe();
int ccache_ncached_elements_unsafe();

#endif /* JEMALLOC_INTERNAL_CCACHE_H */
