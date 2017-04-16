#define JEMALLOC_EXTENT_MMAP_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

/******************************************************************************/

void *
extent_alloc_mmap(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit) {
	void *ret = pages_map(new_addr, size, ALIGNMENT_CEILING(alignment,
	    PAGE), commit);
	if (ret == NULL) {
		return NULL;
	}
	assert(ret != NULL);
	*zero = true;
	return ret;
}

bool
extent_dalloc_mmap(void *addr, size_t size) {
	if (config_munmap) {
		pages_unmap(addr, size);
	}
	return !config_munmap;
}
