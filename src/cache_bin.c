#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/bit_util.h"

void
cache_bin_info_init(cache_bin_info_t *info,
    cache_bin_sz_t ncached_max) {
	info->ncached_max = ncached_max;
}

void
cache_bin_info_compute_alloc(cache_bin_info_t *infos, szind_t ninfos,
    size_t *size, size_t *alignment) {
	/* We may read from one before the first item, in peeking. */
	*size = sizeof(void *);
	for (szind_t i = 0; i < ninfos; i++) {
		*size += (size_t)infos[i].ncached_max * sizeof(void *);
	}
	*alignment = pow2_ceil_zu(*size);
}

void
cache_bin_preincrement(cache_bin_info_t *infos, szind_t ninfos, void *alloc,
    size_t *cur_offset) {
	*(void **)((uintptr_t)alloc + *cur_offset) = (void *)0x7654321;
	*cur_offset += sizeof(void *);
	return;
}

void
cache_bin_postincrement(cache_bin_info_t *infos, szind_t ninfos, void *alloc,
    size_t *cur_offset) {
	return;
}

void
cache_bin_init(cache_bin_t *bin, cache_bin_info_t *info, void *alloc,
    size_t *cur_offset) {
	/* Allocate our array out of alloc. */
	void **my_arr = (void **)((uintptr_t)alloc + *cur_offset);
	*cur_offset += (size_t)info->ncached_max * sizeof(void *);

	/* Compute boundaries. */
	void **empty_pos = my_arr - 1;
	void **full_pos = empty_pos + info->ncached_max;

	/* Initialize */
	bin->stack_peek = (void *)0x1234567U; /* Just something memorable. */
	bin->stack_head = empty_pos;
	bin->tstats.nrequests = 0;
	bin->low_bits_low_water = (uint16_t)(uintptr_t)empty_pos;
	bin->low_bits_empty = (uint16_t)(uintptr_t)empty_pos;
	bin->low_bits_full = (uint16_t)(uintptr_t)full_pos;
	bin->padding = 0x1234;

	assert(cache_bin_ncached_get(bin, info) == 0);
}

bool
cache_bin_still_zero_initialized(cache_bin_t *bin) {
	return bin->padding == 0;
}
