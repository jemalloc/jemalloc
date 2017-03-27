#ifndef JEMALLOC_INTERNAL_BITMAP_TYPES_H
#define JEMALLOC_INTERNAL_BITMAP_TYPES_H

/* Maximum bitmap bit count is 2^LG_BITMAP_MAXBITS. */
#if LG_SLAB_MAXREGS > LG_CEIL_NSIZES
/* Maximum bitmap bit count is determined by maximum regions per slab. */
#  define LG_BITMAP_MAXBITS	LG_SLAB_MAXREGS
#else
/* Maximum bitmap bit count is determined by number of extent size classes. */
#  define LG_BITMAP_MAXBITS	LG_CEIL_NSIZES
#endif
#define BITMAP_MAXBITS		(ZU(1) << LG_BITMAP_MAXBITS)

typedef struct bitmap_level_s bitmap_level_t;
typedef struct bitmap_info_s bitmap_info_t;
typedef unsigned long bitmap_t;
#define LG_SIZEOF_BITMAP	LG_SIZEOF_LONG

/* Number of bits per group. */
#define LG_BITMAP_GROUP_NBITS		(LG_SIZEOF_BITMAP + 3)
#define BITMAP_GROUP_NBITS		(1U << LG_BITMAP_GROUP_NBITS)
#define BITMAP_GROUP_NBITS_MASK		(BITMAP_GROUP_NBITS-1)

/* Number of groups required to store a given number of bits. */
#define BITMAP_BITS2GROUPS(nbits)					\
    (((nbits) + BITMAP_GROUP_NBITS_MASK) >> LG_BITMAP_GROUP_NBITS)

#define BITMAP_GROUPS(nbits)	BITMAP_BITS2GROUPS(nbits)
#define BITMAP_GROUPS_MAX	BITMAP_BITS2GROUPS(BITMAP_MAXBITS)

#define BITMAP_INFO_INITIALIZER(nbits) {				\
	/* nbits. */							\
	nbits,								\
	/* ngroups. */							\
	BITMAP_BITS2GROUPS(nbits)					\
}

#endif /* JEMALLOC_INTERNAL_BITMAP_TYPES_H */
