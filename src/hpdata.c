#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpdata.h"

static int
hpdata_age_comp(const hpdata_t *a, const hpdata_t *b) {
	uint64_t a_age = hpdata_age_get(a);
	uint64_t b_age = hpdata_age_get(b);
	/*
	 * hpdata ages are operation counts in the psset; no two should be the
	 * same.
	 */
	assert(a_age != b_age);
	return (a_age > b_age) - (a_age < b_age);
}

ph_gen(, hpdata_age_heap_, hpdata_age_heap_t, hpdata_t, ph_link, hpdata_age_comp)
