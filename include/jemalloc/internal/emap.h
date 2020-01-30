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

void emap_lock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata);
void emap_unlock_edata(tsdn_t *tsdn, emap_t *emap, edata_t *edata);

void emap_lock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2);
void emap_unlock_edata2(tsdn_t *tsdn, emap_t *emap, edata_t *edata1,
    edata_t *edata2);

edata_t *emap_lock_edata_from_addr(tsdn_t *tsdn, emap_t *emap,
    rtree_ctx_t *rtree_ctx, void *addr, bool inactive_only);

bool emap_rtree_leaf_elms_lookup(tsdn_t *tsdn, emap_t *emap,
    rtree_ctx_t *rtree_ctx, const edata_t *edata, bool dependent,
    bool init_missing, rtree_leaf_elm_t **r_elm_a, rtree_leaf_elm_t **r_elm_b);

/* Only temporarily public; this will be internal eventually. */
void emap_rtree_write_acquired(tsdn_t *tsdn, emap_t *emap,
    rtree_leaf_elm_t *elm_a, rtree_leaf_elm_t *elm_b, edata_t *edata,
    szind_t szind, bool slab);

#endif /* JEMALLOC_INTERNAL_EMAP_H */
