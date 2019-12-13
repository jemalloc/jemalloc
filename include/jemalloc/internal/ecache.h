#ifndef JEMALLOC_INTERNAL_ECACHE_H
#define JEMALLOC_INTERNAL_ECACHE_H

#include "jemalloc/internal/eset.h"
#include "jemalloc/internal/mutex.h"

typedef struct ecache_s ecache_t;
struct ecache_s {
	malloc_mutex_t mtx;
	eset_t eset;
	/* All stored extents must be in the same state. */
	extent_state_t state;
	/* The index of the ehooks the ecache is associated with. */
	unsigned ind;
	/*
	 * If true, delay coalescing until eviction; otherwise coalesce during
	 * deallocation.
	 */
	bool delay_coalesce;
};

typedef struct ecache_grow_s ecache_grow_t;
struct ecache_grow_s {
	/*
	 * Next extent size class in a growing series to use when satisfying a
	 * request via the extent hooks (only if opt_retain).  This limits the
	 * number of disjoint virtual memory ranges so that extent merging can
	 * be effective even if multiple arenas' extent allocation requests are
	 * highly interleaved.
	 *
	 * retain_grow_limit is the max allowed size ind to expand (unless the
	 * required size is greater).  Default is no limit, and controlled
	 * through mallctl only.
	 *
	 * Synchronization: extent_grow_mtx
	 */
	pszind_t next;
	pszind_t limit;
	malloc_mutex_t mtx;
};

static inline size_t
ecache_npages_get(ecache_t *ecache) {
	return eset_npages_get(&ecache->eset);
}
/* Get the number of extents in the given page size index. */
static inline size_t
ecache_nextents_get(ecache_t *ecache, pszind_t ind) {
	return eset_nextents_get(&ecache->eset, ind);
}
/* Get the sum total bytes of the extents in the given page size index. */
static inline size_t
ecache_nbytes_get(ecache_t *ecache, pszind_t ind) {
	return eset_nbytes_get(&ecache->eset, ind);
}

static inline unsigned
ecache_ind_get(ecache_t *ecache) {
	return ecache->ind;
}

bool ecache_init(tsdn_t *tsdn, ecache_t *ecache, extent_state_t state,
    unsigned ind, bool delay_coalesce);
void ecache_prefork(tsdn_t *tsdn, ecache_t *ecache);
void ecache_postfork_parent(tsdn_t *tsdn, ecache_t *ecache);
void ecache_postfork_child(tsdn_t *tsdn, ecache_t *ecache);

bool ecache_grow_init(tsdn_t *tsdn, ecache_grow_t *ecache_grow);
void ecache_grow_prefork(tsdn_t *tsdn, ecache_grow_t *ecache_grow);
void ecache_grow_postfork_parent(tsdn_t *tsdn, ecache_grow_t *ecache_grow);
void ecache_grow_postfork_child(tsdn_t *tsdn, ecache_grow_t *ecache_grow);

#endif /* JEMALLOC_INTERNAL_ECACHE_H */
