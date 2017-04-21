#ifndef JEMALLOC_INTERNAL_BITMAP_INLINES_H
#define JEMALLOC_INTERNAL_BITMAP_INLINES_H

#include "jemalloc/internal/bit_util.h"

static inline bool
bitmap_full(bitmap_t *bitmap, const bitmap_info_t *binfo) {
#ifdef BITMAP_USE_TREE
	size_t rgoff = binfo->levels[binfo->nlevels].group_offset - 1;
	bitmap_t rg = bitmap[rgoff];
	/* The bitmap is full iff the root group is 0. */
	return (rg == 0);
#else
	size_t i;

	for (i = 0; i < binfo->ngroups; i++) {
		if (bitmap[i] != 0) {
			return false;
		}
	}
	return true;
#endif
}

static inline bool
bitmap_get(bitmap_t *bitmap, const bitmap_info_t *binfo, size_t bit) {
	size_t goff;
	bitmap_t g;

	assert(bit < binfo->nbits);
	goff = bit >> LG_BITMAP_GROUP_NBITS;
	g = bitmap[goff];
	return !(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
}

static inline void
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
#ifdef BITMAP_USE_TREE
	/* Propagate group state transitions up the tree. */
	if (g == 0) {
		unsigned i;
		for (i = 1; i < binfo->nlevels; i++) {
			bit = goff;
			goff = bit >> LG_BITMAP_GROUP_NBITS;
			gp = &bitmap[binfo->levels[i].group_offset + goff];
			g = *gp;
			assert(g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)));
			g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
			*gp = g;
			if (g != 0) {
				break;
			}
		}
	}
#endif
}

/* ffu: find first unset >= bit. */
static inline size_t
bitmap_ffu(const bitmap_t *bitmap, const bitmap_info_t *binfo, size_t min_bit) {
	assert(min_bit < binfo->nbits);

#ifdef BITMAP_USE_TREE
	size_t bit = 0;
	for (unsigned level = binfo->nlevels; level--;) {
		size_t lg_bits_per_group = (LG_BITMAP_GROUP_NBITS * (level +
		    1));
		bitmap_t group = bitmap[binfo->levels[level].group_offset + (bit
		    >> lg_bits_per_group)];
		unsigned group_nmask = ((min_bit > bit) ? (min_bit - bit) : 0)
		    >> (lg_bits_per_group - LG_BITMAP_GROUP_NBITS);
		assert(group_nmask <= BITMAP_GROUP_NBITS);
		bitmap_t group_mask = ~((1LU << group_nmask) - 1);
		bitmap_t group_masked = group & group_mask;
		if (group_masked == 0LU) {
			if (group == 0LU) {
				return binfo->nbits;
			}
			/*
			 * min_bit was preceded by one or more unset bits in
			 * this group, but there are no other unset bits in this
			 * group.  Try again starting at the first bit of the
			 * next sibling.  This will recurse at most once per
			 * non-root level.
			 */
			size_t sib_base = bit + (1U << lg_bits_per_group);
			assert(sib_base > min_bit);
			assert(sib_base > bit);
			if (sib_base >= binfo->nbits) {
				return binfo->nbits;
			}
			return bitmap_ffu(bitmap, binfo, sib_base);
		}
		bit += (ffs_lu(group_masked) - 1) << (lg_bits_per_group -
		    LG_BITMAP_GROUP_NBITS);
	}
	assert(bit >= min_bit);
	assert(bit < binfo->nbits);
	return bit;
#else
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
#endif
}

/* sfu: set first unset. */
static inline size_t
bitmap_sfu(bitmap_t *bitmap, const bitmap_info_t *binfo) {
	size_t bit;
	bitmap_t g;
	unsigned i;

	assert(!bitmap_full(bitmap, binfo));

#ifdef BITMAP_USE_TREE
	i = binfo->nlevels - 1;
	g = bitmap[binfo->levels[i].group_offset];
	bit = ffs_lu(g) - 1;
	while (i > 0) {
		i--;
		g = bitmap[binfo->levels[i].group_offset + bit];
		bit = (bit << LG_BITMAP_GROUP_NBITS) + (ffs_lu(g) - 1);
	}
#else
	i = 0;
	g = bitmap[0];
	while ((bit = ffs_lu(g)) == 0) {
		i++;
		g = bitmap[i];
	}
	bit = (i << LG_BITMAP_GROUP_NBITS) + (bit - 1);
#endif
	bitmap_set(bitmap, binfo, bit);
	return bit;
}

static inline void
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
#ifdef BITMAP_USE_TREE
	/* Propagate group state transitions up the tree. */
	if (propagate) {
		unsigned i;
		for (i = 1; i < binfo->nlevels; i++) {
			bit = goff;
			goff = bit >> LG_BITMAP_GROUP_NBITS;
			gp = &bitmap[binfo->levels[i].group_offset + goff];
			g = *gp;
			propagate = (g == 0);
			assert((g & (ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK)))
			    == 0);
			g ^= ZU(1) << (bit & BITMAP_GROUP_NBITS_MASK);
			*gp = g;
			if (!propagate) {
				break;
			}
		}
	}
#endif /* BITMAP_USE_TREE */
}

#endif /* JEMALLOC_INTERNAL_BITMAP_INLINES_H */
