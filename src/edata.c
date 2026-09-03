#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/bin_info.h"
#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/edata.h"

#ifdef DYNAMIC_PAGE_SIZE
size_t edata_alloc_size;

void
edata_boot(void) {
	/*
	 * Large extents use e_prof_info rather than a bitmap, so the tail can
	 * never be smaller than that.
	 */
	size_t tail = sizeof(e_prof_info_t);
	for (szind_t i = 0; i < SC_NBINS; i++) {
		size_t bitmap_bytes = bitmap_size(&bin_infos[i].bitmap_info);
		if (bitmap_bytes > tail) {
			tail = bitmap_bytes;
		}
	}
	edata_alloc_size = offsetof(edata_t, e_slab_data) + tail;
	assert(edata_alloc_size <= sizeof(edata_t));
}
#endif /* DYNAMIC_PAGE_SIZE */

ph_gen(, edata_avail, edata_t, avail_link, edata_esnead_comp)
    ph_gen(, edata_heap, edata_t, heap_link, edata_snad_comp)
