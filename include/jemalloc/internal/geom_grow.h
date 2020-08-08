#ifndef JEMALLOC_INTERNAL_ECACHE_GROW_H
#define JEMALLOC_INTERNAL_ECACHE_GROW_H

typedef struct geom_grow_s geom_grow_t;
struct geom_grow_s {
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
	 * Synchronization: mtx
	 */
	pszind_t next;
	pszind_t limit;
	malloc_mutex_t mtx;
};

bool geom_grow_init(tsdn_t *tsdn, geom_grow_t *geom_grow);
void geom_grow_prefork(tsdn_t *tsdn, geom_grow_t *geom_grow);
void geom_grow_postfork_parent(tsdn_t *tsdn, geom_grow_t *geom_grow);
void geom_grow_postfork_child(tsdn_t *tsdn, geom_grow_t *geom_grow);

#endif /* JEMALLOC_INTERNAL_ECACHE_GROW_H */
