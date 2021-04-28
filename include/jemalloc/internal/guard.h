#ifndef JEMALLOC_INTERNAL_GUARD_H
#define JEMALLOC_INTERNAL_GUARD_H

#include "jemalloc/internal/ehooks.h"

#define PAGE_GUARDS_SIZE (2 * PAGE)

#define SAN_GUARD_LARGE_N_EXTENTS 1
#define SAN_GUARD_SMALL_N_EXTENTS 1

void guard_pages(tsdn_t *tsdn, edata_t *edata, size_t size);
void unguard_pages(tsdn_t *tsdn, edata_t *edata);
void tsd_san_init(tsd_t *tsd);

static inline bool
large_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks, size_t size,
    size_t alignment) {
	if (opt_san_guard_large_nextents == 0 || !ehooks_are_default(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_guard_large_nextent_get(tsd);
	assert(n >= 1);
	if (n > 1) {
		/*
		 * Subtract conditionally because the guard may not happen due
		 * to alignment or size restriction below.
		 */
		*tsd_san_guard_large_nextentp_get(tsd) = n - 1;
	}

	if (n == 1 && (alignment <= PAGE) &&
	    (size + PAGE_GUARDS_SIZE <= SC_LARGE_MAXCLASS)) {
		*tsd_san_guard_large_nextentp_get(tsd) =
		    opt_san_guard_large_nextents;
		return true;
	} else {
		assert(tsd_san_guard_large_nextent_get(tsd) >= 1);
		return false;
	}
}

static inline bool
slab_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks) {
	if (opt_san_guard_small_nextents == 0 || !ehooks_are_default(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_guard_small_nextent_get(tsd);
	assert(n >= 1);
	if (n == 1) {
		*tsd_san_guard_small_nextentp_get(tsd) =
		    opt_san_guard_small_nextents;
		return true;
	} else {
		*tsd_san_guard_small_nextentp_get(tsd) = n - 1;
		assert(tsd_san_guard_small_nextent_get(tsd) >= 1);
		return false;
	}
}

#endif /* JEMALLOC_INTERNAL_GUARD_H */
