#define	JEMALLOC_EXTENT_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

JEMALLOC_INLINE_C int
extent_szad_comp(extent_node_t *a, extent_node_t *b)
{
	int ret;
	size_t a_size = extent_node_size_get(a);
	size_t b_size = extent_node_size_get(b);
	/*
	 * Compute the index of the largest size class that the chunk can
	 * satisfy a request for.
	 */
	size_t a_index = size2index(a_size + 1) - 1;
	size_t b_index = size2index(b_size + 1) - 1;

	ret = (a_index > b_index) - (a_index < b_index);
	if (ret == 0) {
		uintptr_t a_addr = (uintptr_t)extent_node_addr_get(a);
		uintptr_t b_addr = (uintptr_t)extent_node_addr_get(b);

		ret = (a_addr > b_addr) - (a_addr < b_addr);
	}

	return (ret);
}

/* Generate red-black tree functions. */
rb_gen(, extent_tree_szad_, extent_tree_t, extent_node_t, szad_link,
    extent_szad_comp)

JEMALLOC_INLINE_C int
extent_ad_comp(extent_node_t *a, extent_node_t *b)
{
	uintptr_t a_addr = (uintptr_t)extent_node_addr_get(a);
	uintptr_t b_addr = (uintptr_t)extent_node_addr_get(b);

	return ((a_addr > b_addr) - (a_addr < b_addr));
}

/* Generate red-black tree functions. */
rb_gen(, extent_tree_ad_, extent_tree_t, extent_node_t, ad_link, extent_ad_comp)
