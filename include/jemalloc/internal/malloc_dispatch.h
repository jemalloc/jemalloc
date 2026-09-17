#ifndef JEMALLOC_INTERNAL_MALLOC_DISPATCH_H
#define JEMALLOC_INTERNAL_MALLOC_DISPATCH_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/tsd_types.h"

/* Forward decls; only used as pointer types below. */
typedef struct arena_s  arena_t;
typedef struct tcache_s tcache_t;

void *malloc_dispatch_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero, bool slab, tcache_t *tcache);
void  malloc_dispatch_dalloc_promoted(
     tsdn_t *tsdn, void *ptr, tcache_t *tcache, bool slow_path);
void *malloc_dispatch_ralloc(tsdn_t *tsdn, arena_t *arena, void *ptr,
    size_t oldsize, size_t size, size_t alignment, bool zero, bool slab,
    tcache_t *tcache);

#endif /* JEMALLOC_INTERNAL_MALLOC_DISPATCH_H */
