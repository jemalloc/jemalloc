#ifndef JEMALLOC_INTERNAL_EMAP_H
#define JEMALLOC_INTERNAL_EMAP_H

#include "jemalloc/internal/mutex_pool.h"
#include "jemalloc/internal/rtree.h"

typedef struct emap_s emap_t;
struct emap_s {
	rtree_t rtree;
	/* Keyed by the address of the edata_t being protected. */
	mutex_pool_t mtx_pool;
};

extern emap_t emap_global;

bool emap_init(emap_t *emap);

/*
 * Grab the lock or locks associated with the edata or edatas indicated (which
 * is done just by simple address hashing).  The hashing strategy means that
 * it's never safe to grab locks incrementally -- you have to grab all the locks
 * you'll need at once, and release them all at once.
 */
void emap_lock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
void emap_unlock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
void emap_lock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2);
void emap_unlock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2);
edata_t *emap_lock_edata_from_addr(tsdn_t *tsdn, emap_t *emap, void *addr,
    bool inactive_only);

/*
 * Associate the given edata with its beginning and end address, setting the
 * szind and slab info appropriately.
 * Returns true on error (i.e. resource exhaustion).
 */
bool emap_register_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind, bool slab);

/*
 * Does the same thing, but with the interior of the range, for slab
 * allocations.
 *
 * You might wonder why we don't just have a single emap_register function that
 * does both depending on the value of 'slab'.  The answer is twofold:
 * - As a practical matter, in places like the extract->split->commit pathway,
 *   we defer the interior operation until we're sure that the commit won't fail
 *   (but we have to register the split boundaries there).
 * - In general, we're trying to move to a world where the page-specific
 *   allocator doesn't know as much about how the pages it allocates will be
 *   used, and passing a 'slab' parameter everywhere makes that more
 *   complicated.
 *
 * Unlike the boundary version, this function can't fail; this is because slabs
 * can't get big enough to touch a new page that neither of the boundaries
 * touched, so no allocation is necessary to fill the interior once the boundary
 * has been touched.
 */
void emap_register_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind);

void emap_deregister_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
void emap_deregister_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata);

typedef struct emap_prepare_s emap_prepare_t;
struct emap_prepare_s {
	rtree_leaf_elm_t *lead_elm_a;
	rtree_leaf_elm_t *lead_elm_b;
	rtree_leaf_elm_t *trail_elm_a;
	rtree_leaf_elm_t *trail_elm_b;
};

/**
 * These functions do some of the metadata management for merging, splitting,
 * and reusing extents.  In particular, they set the boundary mappings from
 * addresses to edatas and fill in the szind, size, and slab values for the
 * output edata (and, for splitting, *all* values for the trail).  If the result
 * is going to be used as a slab, you still need to call emap_register_interior
 * on it, though.
 *
 * Each operation has a "prepare" and a "commit" portion.  The prepare portion
 * does the operations that can be done without exclusive access to the extent
 * in question, while the commit variant requires exclusive access to maintain
 * the emap invariants.  The only function that can fail is emap_split_prepare,
 * and it returns true on failure (at which point the caller shouldn't commit).
 *
 * In all cases, "lead" refers to the lower-addressed extent, and trail to the
 * higher-addressed one.  Trail can contain garbage (except for its arena_ind
 * and esn values) data for the split variants, and can be reused for any
 * purpose by its given arena after a merge or a failed split.
 */
void emap_remap(tsdn_t *tsdn, emap_t *emap, edata_t *edata, size_t size,
    szind_t szind, bool slab);
bool emap_split_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a, edata_t *trail,
    size_t size_b, szind_t szind_b, bool slab_b);
void emap_split_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, size_t size_a, szind_t szind_a, bool slab_a, edata_t *trail,
    size_t size_b, szind_t szind_b, bool slab_b);
void emap_merge_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail);
void emap_merge_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail);

/* Assert that the emap's view of the given edata matches the edata's view. */
void emap_do_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
static inline void
emap_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	if (config_debug) {
		emap_do_assert_mapped(tsdn, emap, edata);
	}
}

JEMALLOC_ALWAYS_INLINE edata_t *
emap_lookup(tsdn_t *tsdn, emap_t *emap, const void *ptr) {
	rtree_ctx_t rtree_ctx_fallback;
	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

	return rtree_edata_read(tsdn, &emap->rtree, rtree_ctx, (uintptr_t)ptr,
	    true);
}

#endif /* JEMALLOC_INTERNAL_EMAP_H */
