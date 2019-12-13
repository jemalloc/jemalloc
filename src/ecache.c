#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
ecache_init(tsdn_t *tsdn, ecache_t *ecache, extent_state_t state,
    bool delay_coalesce) {
	if (malloc_mutex_init(&ecache->mtx, "extents", WITNESS_RANK_EXTENTS,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	ecache->state = state;
	ecache->delay_coalesce = delay_coalesce;
	eset_init(&ecache->eset, state);
	return false;
}

void
ecache_prefork(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_prefork(tsdn, &ecache->mtx);
}

void
ecache_postfork_parent(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_postfork_parent(tsdn, &ecache->mtx);
}

void
ecache_postfork_child(tsdn_t *tsdn, ecache_t *ecache) {
	malloc_mutex_postfork_child(tsdn, &ecache->mtx);
}

bool
ecache_grow_init(tsdn_t *tsdn, ecache_grow_t *ecache_grow) {
	ecache_grow->next = sz_psz2ind(HUGEPAGE);
	ecache_grow->limit = sz_psz2ind(SC_LARGE_MAXCLASS);
	if (malloc_mutex_init(&ecache_grow->mtx, "extent_grow",
	    WITNESS_RANK_EXTENT_GROW, malloc_mutex_rank_exclusive)) {
		return true;
	}
	return false;
}

void
ecache_grow_prefork(tsdn_t *tsdn, ecache_grow_t *ecache_grow) {
	malloc_mutex_prefork(tsdn, &ecache_grow->mtx);
}

void
ecache_grow_postfork_parent(tsdn_t *tsdn, ecache_grow_t *ecache_grow) {
	malloc_mutex_postfork_parent(tsdn, &ecache_grow->mtx);
}

void
ecache_grow_postfork_child(tsdn_t *tsdn, ecache_grow_t *ecache_grow) {
	malloc_mutex_postfork_child(tsdn, &ecache_grow->mtx);
}
