#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
edata_cache_init(edata_cache_t *edata_cache, base_t *base) {
	edata_avail_new(&edata_cache->avail);
	/*
	 * This is not strictly necessary, since the edata_cache_t is only
	 * created inside an arena, which is zeroed on creation.  But this is
	 * handy as a safety measure.
	 */
	atomic_store_zu(&edata_cache->count, 0, ATOMIC_RELAXED);
	if (malloc_mutex_init(&edata_cache->mtx, "edata_cache",
	    WITNESS_RANK_EDATA_CACHE, malloc_mutex_rank_exclusive)) {
		return true;
	}
	edata_cache->base = base;
	return false;
}

edata_t *
edata_cache_get(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_t *edata = edata_avail_first(&edata_cache->avail);
	if (edata == NULL) {
		malloc_mutex_unlock(tsdn, &edata_cache->mtx);
		return base_alloc_edata(tsdn, edata_cache->base);
	}
	edata_avail_remove(&edata_cache->avail, edata);
	atomic_fetch_sub_zu(&edata_cache->count, 1, ATOMIC_RELAXED);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
	return edata;
}

void
edata_cache_put(tsdn_t *tsdn, edata_cache_t *edata_cache, edata_t *edata) {
	malloc_mutex_lock(tsdn, &edata_cache->mtx);
	edata_avail_insert(&edata_cache->avail, edata);
	atomic_fetch_add_zu(&edata_cache->count, 1, ATOMIC_RELAXED);
	malloc_mutex_unlock(tsdn, &edata_cache->mtx);
}

void
edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_prefork(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_parent(tsdn, &edata_cache->mtx);
}

void
edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_child(tsdn, &edata_cache->mtx);
}

void
edata_cache_small_init(edata_cache_small_t *ecs, edata_cache_t *fallback) {
	edata_list_init(&ecs->list);
	ecs->count = 0;
	ecs->fallback = fallback;
}

edata_t *
edata_cache_small_get(edata_cache_small_t *ecs) {
	assert(ecs->count > 0);
	edata_t *edata = edata_list_first(&ecs->list);
	assert(edata != NULL);
	edata_list_remove(&ecs->list, edata);
	ecs->count--;
	return edata;
}

void
edata_cache_small_put(edata_cache_small_t *ecs, edata_t *edata) {
	assert(edata != NULL);
	edata_list_append(&ecs->list, edata);
	ecs->count++;
}

bool edata_cache_small_prepare(tsdn_t *tsdn, edata_cache_small_t *ecs,
    size_t num) {
	while (ecs->count < num) {
		/*
		 * Obviously, we can be smarter here and batch the locking that
		 * happens inside of edata_cache_get.  But for now, something
		 * quick-and-dirty is fine.
		 */
		edata_t *edata = edata_cache_get(tsdn, ecs->fallback);
		if (edata == NULL) {
			return true;
		}
		ql_elm_new(edata, ql_link);
		edata_cache_small_put(ecs, edata);
	}
	return false;
}

void edata_cache_small_finish(tsdn_t *tsdn, edata_cache_small_t *ecs,
    size_t num) {
	while (ecs->count > num) {
		/* Same deal here -- we should be batching. */
		edata_t *edata = edata_cache_small_get(ecs);
		edata_cache_put(tsdn, ecs->fallback, edata);
	}
}
