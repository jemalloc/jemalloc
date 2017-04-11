#ifndef JEMALLOC_INTERNAL_BITMAP_INLINES_H
#define JEMALLOC_INTERNAL_BITMAP_INLINES_H

#include "jemalloc/internal/bit_util.h"

#ifndef JEMALLOC_ENABLE_INLINE
bool bitmap_full(bitmap_t *bitmap, const bitmap_info_t *binfo);
bool bitmap_get(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit);
void bitmap_set(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit);
size_t bitmap_ffu(const bitmap_t *bitmap, const bitmap_info_t *binfo,
    size_t min_bit);
size_t bitmap_sfu(bitmap_t *bitmap, const bitmap_info_t *binfo);
void bitmap_unset(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_BITMAP_C_))
JEMALLOC_INLINE bool
bitmap_full(bitmap_t *bitmap, const bitmap_info_t *binfo) {
	size_t i;

	for (i = 0; i < binfo->ngroups; i++) {
		if (bitmap[i] != 0) {
			return false;
		}
	}
	return true;
}

JEMALLOC_INLINE bool
bitmap_get(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit) {
	size_t goff;
	bitmap_t g;

	assert(bit < binfo->nbits);
	goff = bit >> LG_BITMAP_GROUP_NBITS;
	g = bitmap[goff];
	return !(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
}

JEMALLOC_INLINE void
bitmap_set(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit) {
	size_t goff;
	bitmap_t *gp;
	bitmap_t g;

	assert(bit < binfo->nbits);
	assert(!bitmap_get(bitmap, binfo, bit));
	goff = bit >> LG_BITMAP_GROUP_NBITS;
	gp = &bitmap[goff];
	g = *gp;
	assert(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
	g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
	*gp = g;
	assert(bitmap_get(bitmap, binfo, bit));
}

/* ffu: find first unset >= bit. */
JEMALLOC_INLINE size_t
bitmap_ffu(const bitmap_t *bitmap, const bitmap_info_t *binfo, size_t min_bit) {
	assert(min_bit < binfo->nbits);

	size_t i = min_bit >> LG_BITMAP_GROUP_NBITS;
	bitmap_t g = bitmap[i] & ~((1LU << (min_bit & BITMAP_GROUP_NBITS_MASK))
	    - 1);
	size_t bit;
	do {
		bit = ffs_lu(g);
		if (bit != 0) {
			return (i << LG_BITMAP_GROUP_NBITS) + (bit - 1);
		}
		i++;
		g = bitmap[i];
	} while (i < binfo->ngroups);
	return binfo->nbits;
}

/* sfu: set first unset. */
JEMALLOC_INLINE size_t
bitmap_sfu(bitmap_t *bitmap, const bitmap_info_t *binfo) {
	size_t bit;
	bitmap_t g;
	unsigned i;

	assert(!bitmap_full(bitmap, binfo));

	i = 0;
	g = bitmap[0];
	while ((bit = ffs_lu(g)) == 0) {
		i++;
		g = bitmap[i];
	}
	bit = (i << LG_BITMAP_GROUP_NBITS) + (bit - 1);
	bitmap_set(bitmap, binfo, bit);
	return bit;
}

JEMALLOC_INLINE void
bitmap_unset(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit) {
	size_t goff;
	bitmap_t *gp;
	bitmap_t g;
	UNUSED bool propagate;

	assert(bit < binfo->nbits);
	assert(bitmap_get(bitmap, binfo, bit));
	goff = bit >> LG_BITMAP_GROUP_NBITS;
	gp = &bitmap[goff];
	g = *gp;
	propagate = (g == 0);
	assert((g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK))) == 0);
	g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
	*gp = g;
	assert(!bitmap_get(bitmap, binfo, bit));
}

#endif

#endif /* JEMALLOC_INTERNAL_BITMAP_INLINES_H */
