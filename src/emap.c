#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/emap.h"

emap_t emap_global;

/*
 * Note: Ends without at semicolon, so that
 *     EMAP_DECLARE_RTREE_CTX;
 * in uses will avoid empty-statement warnings.
 */
#define EMAP_DECLARE_RTREE_CTX						\
    rtree_ctx_t rtree_ctx_fallback;					\
    rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback)

enum emap_lock_result_e {
	emap_lock_result_success,
	emap_lock_result_failure,
	emap_lock_result_no_extent
};
typedef enum emap_lock_result_e emap_lock_result_t;

bool
emap_init(emap_t *emap) {
	bool err;
	err = rtree_new(&emap->rtree, true);
	if (err) {
		return true;
	}
	err = mutex_pool_init(&emap->mtx_pool, "emap_mutex_pool",
	    WITNESS_RANK_EMAP);
	if (err) {
		return true;
	}
	return false;
}

void
emap_lock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	assert(edata != NULL);
	mutex_pool_lock(tsdn, &emap->mtx_pool, (uintptr_t)edata);
}

void
emap_unlock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	assert(edata != NULL);
	mutex_pool_unlock(tsdn, &emap->mtx_pool, (uintptr_t)edata);
}

void
emap_lock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2) {
	assert(edata1 != NULL && edata2 != NULL);
	mutex_pool_lock2(tsdn, &emap->mtx_pool, (uintptr_t)edata1,
	    (uintptr_t)edata2);
}

void
emap_unlock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2) {
	assert(edata1 != NULL && edata2 != NULL);
	mutex_pool_unlock2(tsdn, &emap->mtx_pool, (uintptr_t)edata1,
	    (uintptr_t)edata2);
}

static inline emap_lock_result_t
emap_try_lock_rtree_leaf_elm(tsdn_t *tsdn, emap_t *emap, rtree_leaf_elm_t *elm,
    edata_t **result, bool inactive_only) {
	edata_t *edata1 = rtree_leaf_elm_edata_read(tsdn, &emap->rtree,
	    elm, true);

	/* Slab implies active extents and should be skipped. */
	if (edata1 == NULL || (inactive_only && rtree_leaf_elm_slab_read(tsdn,
	    &emap->rtree, elm, true))) {
		return emap_lock_result_no_extent;
	}

	/*
	 * It's possible that the extent changed out from under us, and with it
	 * the leaf->edata mapping.  We have to recheck while holding the lock.
	 */
	emap_lock_edata(tsdn, emap, edata1);
	edata_t *edata2 = rtree_leaf_elm_edata_read(tsdn, &emap->rtree, elm,
	    true);

	if (edata1 == edata2) {
		*result = edata1;
		return emap_lock_result_success;
	} else {
		emap_unlock_edata(tsdn, emap, edata1);
		return emap_lock_result_failure;
	}
}

/*
 * Returns a pool-locked edata_t * if there's one associated with the given
 * address, and NULL otherwise.
 */
edata_t *
emap_lock_edata_from_addr(tsdn_t *tsdn, emap_t *emap, void *addr,
    bool inactive_only) {
	EMAP_DECLARE_RTREE_CTX;
	edata_t *ret = NULL;
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, &emap->rtree,
	    rtree_ctx, (uintptr_t)addr, false, false);
	if (elm == NULL) {
		return NULL;
	}
	emap_lock_result_t lock_result;
	do {
		lock_result = emap_try_lock_rtree_leaf_elm(tsdn, emap, elm,
		    &ret, inactive_only);
	} while (lock_result == emap_lock_result_failure);
	return ret;
}

static bool
emap_rtree_leaf_elms_lookup(tsdn_t *tsdn, emap_t *emap, rtree_ctx_t *rtree_ctx,
    const edata_t *edata, bool dependent, bool init_missing,
    rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b) {
	*r_elm_a = rtree_leaf_elm_lookup(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_a == NULL) {
		return true;
	}
	assert(*r_elm_a != NULL);

	*r_elm_b = rtree_leaf_elm_lookup(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_last_get(edata), dependent, init_missing);
	if (!dependent && *r_elm_b == NULL) {
		return true;
	}
	assert(*r_elm_b != NULL);

	return false;
}

static void
emap_rtree_write_acquired(tsdn_t *tsdn, emap_t *emap, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, edata_t *edata, szind_t szind, bool slab) {
	rtree_leaf_elm_write(tsdn, &emap->rtree, elm_a, edata, szind, slab);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree, elm_b, edata, szind,
		    slab);
	}
}

bool
emap_register_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind, bool slab) {
	EMAP_DECLARE_RTREE_CTX;

	rtree_leaf_elm_t *elm_a, *elm_b;
	bool err = emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, edata,
	    false, true, &elm_a, &elm_b);
	if (err) {
		return true;
	}
	emap_rtree_write_acquired(tsdn, emap, elm_a, elm_b, edata, szind, slab);
	return false;
}

void
emap_register_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata,
    szind_t szind) {
	EMAP_DECLARE_RTREE_CTX;

	assert(edata_slab_get(edata));

	/* Register interior. */
	for (size_t i = 1; i < (edata_size_get(edata) >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &emap->rtree, rtree_ctx,
		    (uintptr_t)edata_base_get(edata) + (uintptr_t)(i <<
		    LG_PAGE), edata, szind, true);
	}
}

void
emap_deregister_boundary(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	EMAP_DECLARE_RTREE_CTX;
	rtree_leaf_elm_t *elm_a, *elm_b;

	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, edata,
	    true, false, &elm_a, &elm_b);
	emap_rtree_write_acquired(tsdn, emap, elm_a, elm_b, NULL, SC_NSIZES,
	    false);
}

void
emap_deregister_interior(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	EMAP_DECLARE_RTREE_CTX;

	assert(edata_slab_get(edata));
	for (size_t i = 1; i < (edata_size_get(edata) >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &emap->rtree, rtree_ctx,
		    (uintptr_t)edata_base_get(edata) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}

void emap_remap(tsdn_t *tsdn, emap_t *emap, edata_t *edata, size_t size,
    szind_t szind, bool slab) {
	EMAP_DECLARE_RTREE_CTX;

	edata_szind_set(edata, szind);
	if (szind != SC_NSIZES) {
		rtree_szind_slab_update(tsdn, &emap->rtree, rtree_ctx,
		    (uintptr_t)edata_addr_get(edata), szind, slab);
		if (slab && edata_size_get(edata) > PAGE) {
			rtree_szind_slab_update(tsdn,
			    &emap->rtree, rtree_ctx,
			    (uintptr_t)edata_past_get(edata) - (uintptr_t)PAGE,
			    szind, slab);
			}
		}

}

bool
emap_split_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *edata, size_t size_a, szind_t szind_a, bool slab_a, edata_t *trail,
    size_t size_b, szind_t szind_b, bool slab_b) {
	EMAP_DECLARE_RTREE_CTX;
	/*
	 * Note that while the trail mostly inherits its attributes from the
	 * extent to be split, it maintains its own arena ind -- this allows
	 * cross-arena edata interactions, such as occur in the range ecache.
	 */
	edata_init(trail, edata_arena_ind_get(trail),
	    (void *)((uintptr_t)edata_base_get(edata) + size_a), size_b,
	    slab_b, szind_b, edata_sn_get(edata), edata_state_get(edata),
	    edata_zeroed_get(edata), edata_committed_get(edata),
	    edata_dumpable_get(edata), EXTENT_NOT_HEAD);

	/*
	 * We use incorrect constants for things like arena ind, zero, dump, and
	 * commit state, and head status.  This is a fake edata_t, used to
	 * facilitate a lookup.
	 */
	edata_t lead;
	edata_init(&lead, 0U, edata_addr_get(edata), size_a, slab_a, szind_a, 0,
	    extent_state_active, false, false, false, EXTENT_NOT_HEAD);

	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, &lead, false, true,
	    &prepare->lead_elm_a, &prepare->lead_elm_b);
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, trail, false, true,
	    &prepare->trail_elm_a, &prepare->trail_elm_b);

	if (prepare->lead_elm_a == NULL || prepare->lead_elm_b == NULL
	    || prepare->trail_elm_a == NULL || prepare->trail_elm_b == NULL) {
		return true;
	}
	return false;
}

void
emap_split_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, size_t size_a, szind_t szind_a, bool slab_a, edata_t *trail,
    size_t size_b, szind_t szind_b, bool slab_b) {
	edata_size_set(lead, size_a);
	edata_szind_set(lead, szind_a);

	emap_rtree_write_acquired(tsdn, emap, prepare->lead_elm_a,
	    prepare->lead_elm_b, lead, szind_a, slab_a);
	emap_rtree_write_acquired(tsdn, emap, prepare->trail_elm_a,
	    prepare->trail_elm_b, trail, szind_b, slab_b);
}

void
emap_merge_prepare(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail) {
	EMAP_DECLARE_RTREE_CTX;
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, lead, true, false,
	    &prepare->lead_elm_a, &prepare->lead_elm_b);
	emap_rtree_leaf_elms_lookup(tsdn, emap, rtree_ctx, trail, true, false,
	    &prepare->trail_elm_a, &prepare->trail_elm_b);
}

void
emap_merge_commit(tsdn_t *tsdn, emap_t *emap, emap_prepare_t *prepare,
    edata_t *lead, edata_t *trail) {
	if (prepare->lead_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree,
		    prepare->lead_elm_b, NULL, SC_NSIZES, false);
	}

	rtree_leaf_elm_t *merged_b;
	if (prepare->trail_elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree,
		    prepare->trail_elm_a, NULL, SC_NSIZES, false);
		merged_b = prepare->trail_elm_b;
	} else {
		merged_b = prepare->trail_elm_a;
	}

	edata_size_set(lead, edata_size_get(lead) + edata_size_get(trail));
	edata_szind_set(lead, SC_NSIZES);
	edata_sn_set(lead, (edata_sn_get(lead) < edata_sn_get(trail)) ?
	    edata_sn_get(lead) : edata_sn_get(trail));
	edata_zeroed_set(lead, edata_zeroed_get(lead)
	    && edata_zeroed_get(trail));

	emap_rtree_write_acquired(tsdn, emap, prepare->lead_elm_a, merged_b,
	    lead, SC_NSIZES, false);
}

void
emap_do_assert_mapped(tsdn_t *tsdn, emap_t *emap, edata_t *edata) {
	EMAP_DECLARE_RTREE_CTX;

	assert(rtree_edata_read(tsdn, &emap->rtree, rtree_ctx,
	    (uintptr_t)edata_base_get(edata), true) == edata);
}
