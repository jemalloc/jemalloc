#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

/*
 * This file is logically part of the PA module.  While pa.c contains the core
 * allocator functionality, this file contains boring integration functionality;
 * things like the pre- and post- fork handlers, and stats merging for CTL
 * refreshes.
 */

void
pa_shard_prefork0(tsdn_t *tsdn, pa_shard_t *shard) {
	malloc_mutex_prefork(tsdn, &shard->decay_dirty.mtx);
	malloc_mutex_prefork(tsdn, &shard->decay_muzzy.mtx);
}

void
pa_shard_prefork2(tsdn_t *tsdn, pa_shard_t *shard) {
	ecache_grow_prefork(tsdn, &shard->ecache_grow);
}

void
pa_shard_prefork3(tsdn_t *tsdn, pa_shard_t *shard) {
	ecache_prefork(tsdn, &shard->ecache_dirty);
	ecache_prefork(tsdn, &shard->ecache_muzzy);
	ecache_prefork(tsdn, &shard->ecache_retained);
}


void
pa_shard_prefork4(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_prefork(tsdn, &shard->edata_cache);
}

void
pa_shard_postfork_parent(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_postfork_parent(tsdn, &shard->edata_cache);
	ecache_postfork_parent(tsdn, &shard->ecache_dirty);
	ecache_postfork_parent(tsdn, &shard->ecache_muzzy);
	ecache_postfork_parent(tsdn, &shard->ecache_retained);
	ecache_grow_postfork_parent(tsdn, &shard->ecache_grow);
	malloc_mutex_postfork_parent(tsdn, &shard->decay_dirty.mtx);
	malloc_mutex_postfork_parent(tsdn, &shard->decay_muzzy.mtx);
}

void
pa_shard_postfork_child(tsdn_t *tsdn, pa_shard_t *shard) {
	edata_cache_postfork_child(tsdn, &shard->edata_cache);
	ecache_postfork_child(tsdn, &shard->ecache_dirty);
	ecache_postfork_child(tsdn, &shard->ecache_muzzy);
	ecache_postfork_child(tsdn, &shard->ecache_retained);
	ecache_grow_postfork_child(tsdn, &shard->ecache_grow);
	malloc_mutex_postfork_child(tsdn, &shard->decay_dirty.mtx);
	malloc_mutex_postfork_child(tsdn, &shard->decay_muzzy.mtx);
}
