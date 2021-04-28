#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/guard.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/tsd.h"

void
guard_pages(tsdn_t *tsdn, edata_t *edata, size_t usize) {
	size_t size_with_guards = usize + PAGE_GUARDS_SIZE;
	assert((usize & PAGE_MASK) == 0);
	assert(edata_size_get(edata) == size_with_guards);

	uintptr_t guard1 = (uintptr_t)edata_base_get(edata);
	uintptr_t addr = guard1 + PAGE;
	uintptr_t guard2 = addr + usize;

	pages_mark_guards((void *)guard1, (void *)guard2);

	assert(edata_state_get(edata) == extent_state_active);
	/* Update the guarded addr and usable size of the edata. */
	edata_size_set(edata, usize);
	edata_addr_set(edata, (void *)addr);
	edata_guarded_set(edata, true);
}

void
unguard_pages(tsdn_t *tsdn, edata_t *edata) {
	size_t size = edata_size_get(edata);
	size_t size_with_guards = size + PAGE_GUARDS_SIZE;

	uintptr_t addr =  (uintptr_t)edata_base_get(edata);
	uintptr_t guard1 = addr - PAGE;
	uintptr_t guard2 = addr + size;

	assert(edata_state_get(edata) == extent_state_active);
	pages_unmark_guards((void *)guard1, (void *)guard2);

	/* Update the true addr and usable size of the edata. */
	edata_size_set(edata, size_with_guards);
	edata_addr_set(edata, (void *)guard1);
	edata_guarded_set(edata, false);
}

void
tsd_san_init(tsd_t *tsd) {
	*tsd_san_guard_small_nextentp_get(tsd) = opt_san_guard_small_nextents;
	*tsd_san_guard_large_nextentp_get(tsd) = opt_san_guard_large_nextents;
}
