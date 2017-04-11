#define JEMALLOC_BITMAP_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

/******************************************************************************/

void
bitmap_info_init(bitmap_info_t *binfo, size_t nbits) {
	assert(nbits > 0);
	assert(nbits <= (ZU(1) << LG_BITMAP_MAXBITS));

	binfo->ngroups = BITMAP_BITS2GROUPS(nbits);
	binfo->nbits = nbits;
}

static size_t
bitmap_info_ngroups(const bitmap_info_t *binfo) {
	return binfo->ngroups;
}

void
bitmap_init(bitmap_t *bitmap, const bitmap_info_t *binfo, bool fill) {
	size_t extra;

	if (fill) {
		memset(bitmap, 0, bitmap_size(binfo));
		return;
	}

	memset(bitmap, 0xffU, bitmap_size(binfo));
	extra = (BITMAP_GROUP_NBITS - (binfo->nbits & BITMAP_GROUP_NBITS_MASK))
	    & BITMAP_GROUP_NBITS_MASK;
	if (extra != 0) {
		bitmap[binfo->ngroups - 1] >>= extra;
	}
}

size_t
bitmap_size(const bitmap_info_t *binfo) {
	return (bitmap_info_ngroups(binfo) << LG_SIZEOF_BITMAP);
}
