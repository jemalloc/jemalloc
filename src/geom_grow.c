#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool
geom_grow_init(tsdn_t *tsdn, geom_grow_t *geom_grow) {
	geom_grow->next = sz_psz2ind(HUGEPAGE);
	geom_grow->limit = sz_psz2ind(SC_LARGE_MAXCLASS);
	if (malloc_mutex_init(&geom_grow->mtx, "extent_grow",
	    WITNESS_RANK_EXTENT_GROW, malloc_mutex_rank_exclusive)) {
		return true;
	}
	return false;
}

void
geom_grow_prefork(tsdn_t *tsdn, geom_grow_t *geom_grow) {
	malloc_mutex_prefork(tsdn, &geom_grow->mtx);
}

void
geom_grow_postfork_parent(tsdn_t *tsdn, geom_grow_t *geom_grow) {
	malloc_mutex_postfork_parent(tsdn, &geom_grow->mtx);
}

void
geom_grow_postfork_child(tsdn_t *tsdn, geom_grow_t *geom_grow) {
	malloc_mutex_postfork_child(tsdn, &geom_grow->mtx);
}

