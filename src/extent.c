#define	JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

#ifdef JEMALLOC_JET
#undef extent_size_quantize_floor
#define	extent_size_quantize_floor JEMALLOC_N(n_extent_size_quantize_floor)
#endif
size_t
extent_size_quantize_floor(size_t size)
{
	size_t ret;
	pszind_t pind;

	assert(size > 0);
	assert(size <= HUGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	assert(size != 0);
	assert(size == PAGE_CEILING(size));

	pind = psz2ind(size - large_pad + 1);
	if (pind == 0) {
		/*
		 * Avoid underflow.  This short-circuit would also do the right
		 * thing for all sizes in the range for which there are
		 * PAGE-spaced size classes, but it's simplest to just handle
		 * the one case that would cause erroneous results.
		 */
		return (size);
	}
	ret = pind2sz(pind - 1) + large_pad;
	assert(ret <= size);
	return (ret);
}
#ifdef JEMALLOC_JET
#undef extent_size_quantize_floor
#define	extent_size_quantize_floor JEMALLOC_N(extent_size_quantize_floor)
extent_size_quantize_t *extent_size_quantize_floor =
    JEMALLOC_N(n_extent_size_quantize_floor);
#endif

#ifdef JEMALLOC_JET
#undef extent_size_quantize_ceil
#define	extent_size_quantize_ceil JEMALLOC_N(n_extent_size_quantize_ceil)
#endif
size_t
extent_size_quantize_ceil(size_t size)
{
	size_t ret;

	assert(size > 0);
	assert(size <= HUGE_MAXCLASS);
	assert((size & PAGE_MASK) == 0);

	ret = extent_size_quantize_floor(size);
	if (ret < size) {
		/*
		 * Skip a quantization that may have an adequately large extent,
		 * because under-sized extents may be mixed in.  This only
		 * happens when an unusual size is requested, i.e. for aligned
		 * allocation, and is just one of several places where linear
		 * search would potentially find sufficiently aligned available
		 * memory somewhere lower.
		 */
		ret = pind2sz(psz2ind(ret - large_pad + 1)) + large_pad;
	}
	return (ret);
}
#ifdef JEMALLOC_JET
#undef extent_size_quantize_ceil
#define	extent_size_quantize_ceil JEMALLOC_N(extent_size_quantize_ceil)
extent_size_quantize_t *extent_size_quantize_ceil =
    JEMALLOC_N(n_extent_size_quantize_ceil);
#endif

JEMALLOC_INLINE_C int
extent_ad_comp(const extent_t *a, const extent_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

/* Generate pairing heap functions. */
ph_gen(, extent_heap_, extent_heap_t, extent_t, ph_link, extent_ad_comp)
