#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/guard.h"
#include "jemalloc/internal/tsd.h"

/* The sanitizer options. */
size_t opt_san_guard_large = SAN_GUARD_LARGE_EVERY_N_EXTENTS_DEFAULT;
size_t opt_san_guard_small = SAN_GUARD_SMALL_EVERY_N_EXTENTS_DEFAULT;

void
guard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata, emap_t *emap) {
	emap_deregister_boundary(tsdn, emap, edata);

	size_t size_with_guards = edata_size_get(edata);
	size_t usize = size_with_guards - PAGE_GUARDS_SIZE;

	uintptr_t guard1 = (uintptr_t)edata_base_get(edata);
	uintptr_t addr = guard1 + PAGE;
	uintptr_t guard2 = addr + usize;

	assert(edata_state_get(edata) == extent_state_active);
	ehooks_guard(tsdn, ehooks, (void *)guard1, (void *)guard2);

	/* Update the guarded addr and usable size of the edata. */
	edata_size_set(edata, usize);
	edata_addr_set(edata, (void *)addr);
	edata_guarded_set(edata, true);

	/* The new boundary will be registered on the pa_alloc path. */
}

static void
unguard_pages_impl(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata, emap_t *emap,
    bool reg_emap) {
	/* Remove the inner boundary which no longer exists. */
	if (reg_emap) {
		assert(edata_state_get(edata) == extent_state_active);
		emap_deregister_boundary(tsdn, emap, edata);
	} else {
		assert(edata_state_get(edata) == extent_state_retained);
	}

	size_t size = edata_size_get(edata);
	size_t size_with_guards = size + PAGE_GUARDS_SIZE;

	uintptr_t addr =  (uintptr_t)edata_base_get(edata);
	uintptr_t guard1 = addr - PAGE;
	uintptr_t guard2 = addr + size;

	ehooks_unguard(tsdn, ehooks, (void *)guard1, (void *)guard2);

	/* Update the true addr and usable size of the edata. */
	edata_size_set(edata, size_with_guards);
	edata_addr_set(edata, (void *)guard1);
	edata_guarded_set(edata, false);

	/*
	 * Then re-register the outer boundary including the guards, if
	 * requested.
	 */
	if (reg_emap) {
		emap_register_boundary(tsdn, emap, edata, SC_NSIZES,
		    /* slab */ false);
	}
}

void
unguard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata, emap_t *emap) {
	unguard_pages_impl(tsdn, ehooks, edata, emap, /* reg_emap */ true);
}

void
unguard_pages_pre_destroy(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap) {
	emap_assert_not_mapped(tsdn, emap, edata);
	unguard_pages_impl(tsdn, ehooks, edata, emap, /* reg_emap */ false);
}

void
tsd_san_init(tsd_t *tsd) {
	*tsd_san_extents_until_guard_smallp_get(tsd) = opt_san_guard_small;
	*tsd_san_extents_until_guard_largep_get(tsd) = opt_san_guard_large;
}
