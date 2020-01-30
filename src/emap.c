#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/emap.h"

emap_t emap_global;

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
emap_lock_edata_from_addr(tsdn_t *tsdn, emap_t *emap, rtree_ctx_t *rtree_ctx,
    void *addr, bool inactive_only) {
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

bool
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

void
emap_rtree_write_acquired(tsdn_t *tsdn, emap_t *emap, rtree_leaf_elm_t *elm_a,
    rtree_leaf_elm_t *elm_b, edata_t *edata, szind_t szind, bool slab) {
	rtree_leaf_elm_write(tsdn, &emap->rtree, elm_a, edata, szind, slab);
	if (elm_b != NULL) {
		rtree_leaf_elm_write(tsdn, &emap->rtree, elm_b, edata, szind,
		    slab);
	}
}
