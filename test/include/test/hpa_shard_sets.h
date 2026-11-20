#ifndef TEST_HPA_SHARD_SETS_H
#define TEST_HPA_SHARD_SETS_H

#include "jemalloc/internal/hpa_shard_set.h"

/*
 * Helpers for the process-wide HPA shard sets, which malloc_init boots from
 * the options in the test's .sh.
 */

/* Creates the shard in slot of set i of the global sets if need be. */
static inline hpa_shard_t *
test_hpa_shard_sets_shard(unsigned i, unsigned slot) {
	assert_u_lt(i, hpa_shard_sets_global.nsets, "no such set");
	hpa_shard_t *shard = hpa_shard_choose(
	    tsd_tsdn(tsd_fetch()), &hpa_shard_sets_global.sets[i], slot);
	assert_ptr_not_null(shard, "creating the shard failed");
	return shard;
}

static inline edata_t *
test_hpa_shard_alloc(hpa_shard_t *shard, size_t size) {
	bool     deferred_work_generated = false;
	edata_t *edata = hpa_alloc(tsd_tsdn(tsd_fetch()), shard, size, PAGE,
	    /* zero */ false, /* guarded */ false, /* frequent_reuse */ false,
	    &deferred_work_generated);
	assert_ptr_not_null(edata, "hpa_alloc(%zu) failed", size);
	return edata;
}

static inline void
test_hpa_shard_dalloc(hpa_shard_t *shard, edata_t *edata) {
	bool deferred_work_generated = false;
	hpa_dalloc(
	    tsd_tsdn(tsd_fetch()), shard, edata, &deferred_work_generated);
}

#endif /* TEST_HPA_SHARD_SETS_H */
