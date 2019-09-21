#ifndef JEMALLOC_INTERNAL_EXTENT_EXTERNS_H
#define JEMALLOC_INTERNAL_EXTENT_EXTERNS_H

#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/mutex_pool.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/rtree.h"

extern size_t opt_lg_extent_max_active_fit;

extern rtree_t extents_rtree;
extern const extent_hooks_t extent_hooks_default;
extern mutex_pool_t extent_mutex_pool;

extent_t *extent_alloc(tsdn_t *tsdn, arena_t *arena);
void extent_dalloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent);

extent_hooks_t *extent_hooks_get(arena_t *arena);
extent_hooks_t *extent_hooks_set(tsd_t *tsd, arena_t *arena,
    extent_hooks_t *extent_hooks);

ph_proto(, extent_avail_, extent_tree_t, extent_t)
ph_proto(, extent_heap_, extent_heap_t, extent_t)

extent_t *extents_alloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, eset_t *eset, void *new_addr,
    size_t size, size_t pad, size_t alignment, bool slab, szind_t szind,
    bool *zero, bool *commit);
void extents_dalloc(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, eset_t *eset, extent_t *extent);
extent_t *extents_evict(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, eset_t *eset, size_t npages_min);
extent_t *extent_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, void *new_addr, size_t size, size_t pad,
    size_t alignment, bool slab, szind_t szind, bool *zero, bool *commit);
void extent_dalloc_gap(tsdn_t *tsdn, arena_t *arena, extent_t *extent);
void extent_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent);
void extent_destroy_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent);
bool extent_commit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool extent_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool extent_purge_lazy_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool extent_purge_forced_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t offset,
    size_t length);
extent_t *extent_split_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *extent, size_t size_a,
    szind_t szind_a, bool slab_a, size_t size_b, szind_t szind_b, bool slab_b);
bool extent_merge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t **r_extent_hooks, extent_t *a, extent_t *b);

bool extent_boot(void);

void extent_util_stats_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size);
void extent_util_stats_verbose_get(tsdn_t *tsdn, const void *ptr,
    size_t *nfree, size_t *nregs, size_t *size,
    size_t *bin_nfree, size_t *bin_nregs, void **slabcur_addr);

#endif /* JEMALLOC_INTERNAL_EXTENT_EXTERNS_H */
