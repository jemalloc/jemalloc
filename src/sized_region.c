#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sized_region.h"

#include "jemalloc/internal/log.h"

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

	for (int i = 0; i < SIZED_REGION_NUM_SCS; i++) {
		if (malloc_mutex_init(&region->bins[i].dumpable_mtx,
		    "sized_region", WITNESS_RANK_SIZED_REGION,
		    malloc_mutex_rank_exclusive)) {
			LOG("sized_region.init.failure.mtx_init",
			    "region is %p, i is %d", region, i);
			return;
		}
	}

	size_t sc_size = (ZU(1) << opt_lg_sized_region_size);
	size_t size = SIZED_REGION_NUM_SCS * sc_size;
	void *mem = pages_map(NULL, size, PAGE, &region->committed);

	if (mem == NULL) {
		LOG("sized_region.init.failure.alloc", "region is %p, "
		    "attempted size was %zu", region, size);
		return;
	}

	if (pages_dontdump(mem, size)) {
		LOG("sized_region.init.failure.dontdump",
		    "region is %p, attempted size was %zu", region, size);
		return;
	}

	/* Success! */
	region->start = (uintptr_t)mem;
	region->size = size;
	LOG("sized_region.init.success",
	    "region is %p, start is %p, size is %zu", region, mem, size);
}

#ifdef JEMALLOC_JET
void
sized_region_destroy(sized_region_t *region) {
	if (region->start != 0) {
		pages_unmap((void *)region->start, region->size);
	}
}
#endif

bool
sized_region_expand_dumpable(tsdn_t *tsdn, sized_region_t *region, size_t size,
    szind_t szind) {
	const size_t sc_size = (ZU(1) << opt_lg_sized_region_size);
	/*
	 * Do dumpability changes under a mutex, to avoid redundant calls and
	 * simplify batching.
	 */
	sized_region_bin_t *bin = &region->bins[szind];

	malloc_mutex_lock(tsdn, &bin->dumpable_mtx);
	size_t cur_dumpable = atomic_load_zu(&bin->dumpable, ATOMIC_RELAXED);
	assert(cur_dumpable >= atomic_load_zu(&bin->used, ATOMIC_RELAXED));
	/* Increase by an extra couple megabytes if we can, to batch calls. */
	size_t increment = cur_dumpable + size + 2 * 1024 * 1024;
	size_t new_dumpable = cur_dumpable + increment;
	if (new_dumpable > sc_size) {
		new_dumpable = sc_size;
		increment = new_dumpable - cur_dumpable;
	}
	void *start = (void *)(region->start + (szind * sc_size)
	    + cur_dumpable);
	bool err = pages_dodump(start, increment);
	if (!err) {
		atomic_store_zu(&bin->dumpable, new_dumpable, ATOMIC_RELAXED);
	}
	malloc_mutex_unlock(tsdn, &region->bins[szind].dumpable_mtx);
	return err;
}

