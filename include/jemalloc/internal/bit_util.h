#ifndef JEMALLOC_INTERNAL_BIT_UTIL_H
#define JEMALLOC_INTERNAL_BIT_UTIL_H

#include "jemalloc/internal/assert.h"

#define BIT_UTIL_INLINE static inline

/* Sanity check. */
#if !defined(JEMALLOC_INTERNAL_FFSLL) || !defined(JEMALLOC_INTERNAL_FFSL) \
    || !defined(JEMALLOC_INTERNAL_FFS)
#  error JEMALLOC_INTERNAL_FFS{,L,LL} should have been defined by configure
#endif


BIT_UTIL_INLINE unsigned
ffs_llu(unsigned long long bitmap) {
	return JEMALLOC_INTERNAL_FFSLL(bitmap);
}

BIT_UTIL_INLINE unsigned
ffs_lu(unsigned long bitmap) {
	return JEMALLOC_INTERNAL_FFSL(bitmap);
}

BIT_UTIL_INLINE unsigned
ffs_u(unsigned bitmap) {
	return JEMALLOC_INTERNAL_FFS(bitmap);
}

BIT_UTIL_INLINE unsigned
ffs_zu(size_t bitmap) {
#if LG_SIZEOF_PTR == LG_SIZEOF_INT
	return ffs_u(bitmap);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG
	return ffs_lu(bitmap);
#elif LG_SIZEOF_PTR == LG_SIZEOF_LONG_LONG
	return ffs_llu(bitmap);
#else
#error No implementation for size_t ffs()
#endif
}

BIT_UTIL_INLINE unsigned
ffs_u64(uint64_t bitmap) {
#if LG_SIZEOF_LONG == 3
	return ffs_lu(bitmap);
#elif LG_SIZEOF_LONG_LONG == 3
	return ffs_llu(bitmap);
#else
#error No implementation for 64-bit ffs()
#endif
}

BIT_UTIL_INLINE unsigned
ffs_u32(uint32_t bitmap) {
#if LG_SIZEOF_INT == 2
	return ffs_u(bitmap);
#else
#error No implementation for 32-bit ffs()
#endif
	return ffs_u(bitmap);
}

BIT_UTIL_INLINE uint64_t
pow2_ceil_u64(uint64_t x) {
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;
	x++;
	return x;
}

BIT_UTIL_INLINE uint32_t
pow2_ceil_u32(uint32_t x) {
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x++;
	return x;
}

/* Compute the smallest power of 2 that is >= x. */
BIT_UTIL_INLINE size_t
pow2_ceil_zu(size_t x) {
#if (LG_SIZEOF_PTR == 3)
	return pow2_ceil_u64(x);
#else
	return pow2_ceil_u32(x);
#endif
}

#if (defined(__i386__) || defined(__amd64__) || defined(__x86_64__))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	size_t ret;
	assert(x != 0);

	asm ("bsr %1, %0"
	    : "=r"(ret) // Outputs.
	    : "r"(x)    // Inputs.
	    );
	assert(ret < UINT_MAX);
	return (unsigned)ret;
}
#elif (defined(_MSC_VER))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	unsigned long ret;

	assert(x != 0);

#if (LG_SIZEOF_PTR == 3)
	_BitScanReverse64(&ret, x);
#elif (LG_SIZEOF_PTR == 2)
	_BitScanReverse(&ret, x);
#else
#  error "Unsupported type size for lg_floor()"
#endif
	assert(ret < UINT_MAX);
	return (unsigned)ret;
}
#elif (defined(JEMALLOC_HAVE_BUILTIN_CLZ))
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	assert(x != 0);

#if (LG_SIZEOF_PTR == LG_SIZEOF_INT)
	return ((8 << LG_SIZEOF_PTR) - 1) - __builtin_clz(x);
#elif (LG_SIZEOF_PTR == LG_SIZEOF_LONG)
	return ((8 << LG_SIZEOF_PTR) - 1) - __builtin_clzl(x);
#else
#  error "Unsupported type size for lg_floor()"
#endif
}
#else
BIT_UTIL_INLINE unsigned
lg_floor(size_t x) {
	assert(x != 0);

	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
#if (LG_SIZEOF_PTR == 3)
	x |= (x >> 32);
#endif
	if (x == SIZE_T_MAX) {
		return (8 << LG_SIZEOF_PTR) - 1;
	}
	x++;
	return ffs_zu(x) - 2;
}
#endif

#undef BIT_UTIL_INLINE

/* A compile-time version of lg_ceil */
#define LG_CEIL(x) (							\
    (x) <= (1ULL << 0ULL) ? 0 :						\
    (x) <= (1ULL << 1ULL) ? 1 :						\
    (x) <= (1ULL << 2ULL) ? 2 :						\
    (x) <= (1ULL << 3ULL) ? 3 :						\
    (x) <= (1ULL << 4ULL) ? 4 :						\
    (x) <= (1ULL << 5ULL) ? 5 :						\
    (x) <= (1ULL << 6ULL) ? 6 :						\
    (x) <= (1ULL << 7ULL) ? 7 :						\
    (x) <= (1ULL << 8ULL) ? 8 :						\
    (x) <= (1ULL << 9ULL) ? 9 :						\
    (x) <= (1ULL << 10ULL) ? 10 :					\
    (x) <= (1ULL << 11ULL) ? 11 :					\
    (x) <= (1ULL << 12ULL) ? 12 :					\
    (x) <= (1ULL << 13ULL) ? 13 :					\
    (x) <= (1ULL << 14ULL) ? 14 :					\
    (x) <= (1ULL << 15ULL) ? 15 :					\
    (x) <= (1ULL << 16ULL) ? 16 :					\
    (x) <= (1ULL << 17ULL) ? 17 :					\
    (x) <= (1ULL << 18ULL) ? 18 :					\
    (x) <= (1ULL << 19ULL) ? 19 :					\
    (x) <= (1ULL << 20ULL) ? 20 :					\
    (x) <= (1ULL << 21ULL) ? 21 :					\
    (x) <= (1ULL << 22ULL) ? 22 :					\
    (x) <= (1ULL << 23ULL) ? 23 :					\
    (x) <= (1ULL << 24ULL) ? 24 :					\
    (x) <= (1ULL << 25ULL) ? 25 :					\
    (x) <= (1ULL << 26ULL) ? 26 :					\
    (x) <= (1ULL << 27ULL) ? 27 :					\
    (x) <= (1ULL << 28ULL) ? 28 :					\
    (x) <= (1ULL << 29ULL) ? 29 :					\
    (x) <= (1ULL << 30ULL) ? 30 :					\
    (x) <= (1ULL << 31ULL) ? 31 :					\
    (x) <= (1ULL << 32ULL) ? 32 :					\
    (x) <= (1ULL << 33ULL) ? 33 :					\
    (x) <= (1ULL << 34ULL) ? 34 :					\
    (x) <= (1ULL << 35ULL) ? 35 :					\
    (x) <= (1ULL << 36ULL) ? 36 :					\
    (x) <= (1ULL << 37ULL) ? 37 :					\
    (x) <= (1ULL << 38ULL) ? 38 :					\
    (x) <= (1ULL << 39ULL) ? 39 :					\
    (x) <= (1ULL << 40ULL) ? 40 :					\
    (x) <= (1ULL << 41ULL) ? 41 :					\
    (x) <= (1ULL << 42ULL) ? 42 :					\
    (x) <= (1ULL << 43ULL) ? 43 :					\
    (x) <= (1ULL << 44ULL) ? 44 :					\
    (x) <= (1ULL << 45ULL) ? 45 :					\
    (x) <= (1ULL << 46ULL) ? 46 :					\
    (x) <= (1ULL << 47ULL) ? 47 :					\
    (x) <= (1ULL << 48ULL) ? 48 :					\
    (x) <= (1ULL << 49ULL) ? 49 :					\
    (x) <= (1ULL << 50ULL) ? 50 :					\
    (x) <= (1ULL << 51ULL) ? 51 :					\
    (x) <= (1ULL << 52ULL) ? 52 :					\
    (x) <= (1ULL << 53ULL) ? 53 :					\
    (x) <= (1ULL << 54ULL) ? 54 :					\
    (x) <= (1ULL << 55ULL) ? 55 :					\
    (x) <= (1ULL << 56ULL) ? 56 :					\
    (x) <= (1ULL << 57ULL) ? 57 :					\
    (x) <= (1ULL << 58ULL) ? 58 :					\
    (x) <= (1ULL << 59ULL) ? 59 :					\
    (x) <= (1ULL << 60ULL) ? 60 :					\
    (x) <= (1ULL << 61ULL) ? 61 :					\
    (x) <= (1ULL << 62ULL) ? 62 :					\
    (x) <= (1ULL << 63ULL) ? 63 :					\
    64)

#endif /* JEMALLOC_INTERNAL_BIT_UTIL_H */
