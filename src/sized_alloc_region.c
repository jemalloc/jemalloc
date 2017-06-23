#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sized_alloc_region.h"

extern sized_alloc_region_t sized_alloc_region_global;

/* Disabled by default. */
bool opt_sized_alloc_region = false;
sized_alloc_region_t sized_alloc_region_global;

void
sized_alloc_region_init(sized_alloc_region_t *region) {
#ifndef JEMALLOC_JET
	/* Should only be one of these in a real program. */
	assert(region == &sized_alloc_region_global);
#endif
	memset(region, '\0', sizeof(*region));
	void *mem = pages_map(NULL, SIZED_ALLOC_REGION_SIZE, PAGE,
	    &region->committed);
	if (mem != NULL) {
		region->region_start = (uintptr_t)mem;
		region->region_size = SIZED_ALLOC_REGION_SIZE;
	}
}

#ifdef JEMALLOC_JET
void
sized_alloc_region_destroy(sized_alloc_region_t *region) {
	if (region->region_start != 0) {
		pages_unmap((void *)region->region_start, region->region_size);
	}
}
#endif
