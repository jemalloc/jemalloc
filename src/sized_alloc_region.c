#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sized_alloc_region.h"

#include "jemalloc/internal/log.h"

extern sized_alloc_region_t sized_alloc_region_global;

/* Disabled by default. */
bool opt_sized_alloc_region = false;

ssize_t opt_sized_alloc_region_lg_sc_size =
#if LG_SIZEOF_PTR == 2
  25
#elif LG_SIZEOF_PTR == 3
  32
#else
#  error Got a weird value of LG_SIZEOF_PTR
  -1
#endif
;

sized_alloc_region_t sized_alloc_region_global;

void
sized_alloc_region_init(sized_alloc_region_t *region) {
#ifndef JEMALLOC_JET
	/* Should only be one of these in a real program. */
	assert(region == &sized_alloc_region_global);
#endif
	memset(region, '\0', sizeof(*region));

	size_t sc_size = (ZU(1) << opt_sized_alloc_region_lg_sc_size);
	size_t size = SIZED_ALLOC_REGION_NUM_SCS * sc_size;
	void *mem = pages_map(NULL, size, PAGE, &region->committed);
	if (mem != NULL) {
		region->region_start = (uintptr_t)mem;
		region->region_size = size;
	}
	log("sized_alloc_region.init", "start is %p, size is %zu", mem, size);
}

#ifdef JEMALLOC_JET
void
sized_alloc_region_destroy(sized_alloc_region_t *region) {
	if (region->region_start != 0) {
		pages_unmap((void *)region->region_start, region->region_size);
	}
}
#endif
