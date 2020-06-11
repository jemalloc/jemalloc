#ifndef JEMALLOC_INTERNAL_EDATA_CACHE_H
#define JEMALLOC_INTERNAL_EDATA_CACHE_H

#include "jemalloc/internal/base.h"

/*
 * A cache of edata_t structures allocated via base_alloc_edata (as opposed to
 * the underlying extents they describe).  The contents of returned edata_t
 * objects are garbage and cannot be relied upon.
 */

typedef struct edata_cache_s edata_cache_t;
struct edata_cache_s {
	edata_tree_t avail;
	atomic_zu_t count;
	malloc_mutex_t mtx;
	base_t *base;
};

bool edata_cache_init(edata_cache_t *edata_cache, base_t *base);
edata_t *edata_cache_get(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_put(tsdn_t *tsdn, edata_cache_t *edata_cache, edata_t *edata);

void edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache);
void edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache);

typedef struct edata_cache_small_s edata_cache_small_t;
struct edata_cache_small_s {
	edata_list_inactive_t list;
	size_t count;
	edata_cache_t *fallback;
};

/*
 * An edata_cache_small is like an edata_cache, but it relies on external
 * synchronization and avoids first-fit strategies.  You can call "prepare" to
 * acquire at least num edata_t objects, and then "finish" to flush all
 * excess ones back to their fallback edata_cache_t.  Once they have been
 * acquired, they can be allocated without failing (and in fact, this is
 * required -- it's not permitted to attempt to get an edata_t without first
 * preparing for it).
 */

void edata_cache_small_init(edata_cache_small_t *ecs, edata_cache_t *fallback);

/* Returns whether or not an error occurred. */
bool edata_cache_small_prepare(tsdn_t *tsdn, edata_cache_small_t *ecs,
    size_t num);
edata_t *edata_cache_small_get(edata_cache_small_t *ecs);

void edata_cache_small_put(edata_cache_small_t *ecs, edata_t *edata);
void edata_cache_small_finish(tsdn_t *tsdn, edata_cache_small_t *ecs,
    size_t num);

#endif /* JEMALLOC_INTERNAL_EDATA_CACHE_H */
