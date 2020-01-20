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

void edata_cache_prefork(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_prefork(tsdn, &edata_cache->mtx);
}

void edata_cache_postfork_parent(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_parent(tsdn, &edata_cache->mtx);
}

void edata_cache_postfork_child(tsdn_t *tsdn, edata_cache_t *edata_cache) {
	malloc_mutex_postfork_child(tsdn, &edata_cache->mtx);
}
