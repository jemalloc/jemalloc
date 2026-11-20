#include "test/jemalloc_test.h"

/*
 * End to end through xallocx(), on a fresh arena without the tcache so that the
 * extent is alone in its pageslab.  Sizes are in pages to work with any page
 * size.
 */
TEST_BEGIN(test_hpa_resize_in_place) {
	/* Zeroed growth is never done in place by the HPA. */
	test_skip_if(!opt_hpa || opt_zero || san_guard_enabled());

	/* Let the HPA serve the sizes below; new arenas copy this. */
	opt_hpa_opts.slab_max_alloc = 16 * PAGE;
	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	int   flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;
	void *p = mallocx(10 * PAGE, flags);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	edata_t *edata = emap_edata_lookup(
	    tsd_tsdn(tsd_fetch()), &arena_emap_global, p);
	expect_d_eq(
	    EXTENT_PAI_HPA, edata_pai_get(edata), "Expected HPA extent");

	expect_zu_eq(12 * PAGE, xallocx(p, 12 * PAGE, 0, flags),
	    "Growth did not happen in place");
	expect_zu_eq(5 * PAGE, xallocx(p, 5 * PAGE, 0, flags),
	    "Shrink did not happen in place");

	/* Installing extent hooks, even the current ones, disables the HPA. */
	char cmd[64];
	malloc_snprintf(cmd, sizeof(cmd), "arena.%u.extent_hooks", arena_ind);
	extent_hooks_t *hooks;
	sz = sizeof(hooks);
	expect_d_eq(mallctl(cmd, (void *)&hooks, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	expect_d_eq(mallctl(cmd, NULL, NULL, (void *)&hooks, sizeof(hooks)), 0,
	    "Unexpected mallctl() failure");
	expect_zu_eq(5 * PAGE, xallocx(p, 8 * PAGE, 0, flags),
	    "Growth after disabling the HPA happened in place");
	expect_zu_eq(4 * PAGE, xallocx(p, 4 * PAGE, 0, flags),
	    "Shrink after disabling the HPA did not happen in place");
	dallocx(p, flags);
}
TEST_END

int
main(void) {
	if (hpa_supported()) {
		opt_hpa = true;
	}
	return test_no_reentrancy(test_hpa_resize_in_place);
}
