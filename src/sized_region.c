#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sized_region.h"

#include "jemalloc/internal/log.h"

extern sized_region_t sized_region_global;

/* Disabled by default. */
bool opt_sized_regions = false;

ssize_t opt_lg_sized_region_size =
#if LG_SIZEOF_PTR == 2
  24
#elif LG_SIZEOF_PTR == 3
  32
#else
#  error Got a weird value of LG_SIZEOF_PTR
  -1
#endif
;

sized_region_t sized_region_global;

void
sized_region_init(sized_region_t *region) {
#ifndef JEMALLOC_JET
	/* Should only be one of these in a real program. */
	assert(region == &sized_region_global);
#endif
	memset(region, '\0', sizeof(*region));

	size_t sc_size = (ZU(1) << opt_lg_sized_region_size);
	size_t size = SIZED_REGION_NUM_SCS * sc_size;
	void *mem = pages_map(NULL, size, PAGE, &region->committed);
	if (mem != NULL) {
		region->start = (uintptr_t)mem;
		region->size = size;
		LOG("sized_region.init.success", "start is %p, size is %zu",
		    mem, size);
	} else {
		LOG("sized_region.init.failure", "attempted size was %zu",
		    size);
	}
}

#ifdef JEMALLOC_JET
void
sized_region_destroy(sized_region_t *region) {
	if (region->start != 0) {
		pages_unmap((void *)region->start, region->size);
	}
}
#endif
