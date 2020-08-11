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
	 */
	pszind_t next;
	pszind_t limit;
};

static inline bool
geom_grow_size_prepare(geom_grow_t *geom_grow, size_t alloc_size_min,
    size_t *r_alloc_size, pszind_t *r_skip) {
	*r_skip = 0;
	*r_alloc_size = sz_pind2sz(geom_grow->next + *r_skip);
	while (*r_alloc_size < alloc_size_min) {
		(*r_skip)++;
		if (geom_grow->next + *r_skip  >=
		    sz_psz2ind(SC_LARGE_MAXCLASS)) {
			/* Outside legal range. */
			return true;
		}
		*r_alloc_size = sz_pind2sz(geom_grow->next + *r_skip);
	}
	return false;
}

static inline void
geom_grow_size_commit(geom_grow_t *geom_grow, pszind_t skip) {
	if (geom_grow->next + skip + 1 <= geom_grow->limit) {
		geom_grow->next += skip + 1;
	} else {
		geom_grow->next = geom_grow->limit;
	}

}

void geom_grow_init(geom_grow_t *geom_grow);

#endif /* JEMALLOC_INTERNAL_ECACHE_GROW_H */
