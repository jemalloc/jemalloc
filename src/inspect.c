#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"
#include "jemalloc/internal/inspect.h"

void
inspect_extent_util_stats_get(
    tsdn_t *tsdn, const void *ptr, size_t *nfree, size_t *nregs, size_t *size) {
	assert(ptr != NULL && nfree != NULL && nregs != NULL && size != NULL);

	const edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	if (unlikely(edata == NULL)) {
		*nfree = *nregs = *size = 0;
		return;
	}

	*size = edata_size_get(edata);
	if (!edata_slab_get(edata)) {
		*nfree = 0;
		*nregs = 1;
	} else {
		*nfree = edata_nfree_get(edata);
		*nregs = bin_infos[edata_szind_get(edata)].nregs;
		assert(*nfree <= *nregs);
		assert(*nfree * edata_usize_get(edata) <= *size);
	}
}
