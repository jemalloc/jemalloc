#ifndef JEMALLOC_INTERNAL_GUARD_H
#define JEMALLOC_INTERNAL_GUARD_H

#include "jemalloc/internal/ehooks.h"
#include "jemalloc/internal/emap.h"

#define SAN_PAGE_GUARD PAGE
#define SAN_PAGE_GUARDS_SIZE (SAN_PAGE_GUARD * 2)

#define SAN_GUARD_LARGE_EVERY_N_EXTENTS_DEFAULT 0
#define SAN_GUARD_SMALL_EVERY_N_EXTENTS_DEFAULT 0

/* 0 means disabled, i.e. never guarded. */
extern size_t opt_san_guard_large;
extern size_t opt_san_guard_small;

void san_guard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right, bool remap);
void san_unguard_pages(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool left, bool right);
/*
 * Unguard the extent, but don't modify emap boundaries. Must be called on an
 * extent that has been erased from emap and shouldn't be placed back.
 */
void san_unguard_pages_pre_destroy(tsdn_t *tsdn, ehooks_t *ehooks,
    edata_t *edata, emap_t *emap);
void tsd_san_init(tsd_t *tsd);

static inline void
san_guard_pages_two_sided(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap, bool remap) {
	return san_guard_pages(tsdn, ehooks, edata, emap, true, true,
	    remap);
}

static inline void
san_unguard_pages_two_sided(tsdn_t *tsdn, ehooks_t *ehooks, edata_t *edata,
    emap_t *emap) {
	return san_unguard_pages(tsdn, ehooks, edata, emap, true, true);
}

static inline size_t
san_two_side_unguarded_sz(size_t size) {
	assert(size % PAGE == 0);
	assert(size >= SAN_PAGE_GUARDS_SIZE);
	return size - SAN_PAGE_GUARDS_SIZE;
}

static inline size_t
san_two_side_guarded_sz(size_t size) {
	assert(size % PAGE == 0);
	return size + SAN_PAGE_GUARDS_SIZE;
}

static inline size_t
san_one_side_unguarded_sz(size_t size) {
	assert(size % PAGE == 0);
	assert(size >= SAN_PAGE_GUARD);
	return size - SAN_PAGE_GUARD;
}

static inline size_t
san_one_side_guarded_sz(size_t size) {
	assert(size % PAGE == 0);
	return size + SAN_PAGE_GUARD;
}

static inline bool
san_enabled(void) {
	return (opt_san_guard_large != 0 || opt_san_guard_small != 0);
}

static inline bool
san_large_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks, size_t size,
    size_t alignment) {
	if (opt_san_guard_large == 0 || ehooks_guard_will_fail(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_extents_until_guard_large_get(tsd);
	assert(n >= 1);
	if (n > 1) {
		/*
		 * Subtract conditionally because the guard may not happen due
		 * to alignment or size restriction below.
		 */
		*tsd_san_extents_until_guard_largep_get(tsd) = n - 1;
	}

	if (n == 1 && (alignment <= PAGE) &&
	    (san_two_side_guarded_sz(size) <= SC_LARGE_MAXCLASS)) {
		*tsd_san_extents_until_guard_largep_get(tsd) =
		    opt_san_guard_large;
		return true;
	} else {
		assert(tsd_san_extents_until_guard_large_get(tsd) >= 1);
		return false;
	}
}

static inline bool
san_slab_extent_decide_guard(tsdn_t *tsdn, ehooks_t *ehooks) {
	if (opt_san_guard_small == 0 || ehooks_guard_will_fail(ehooks) ||
	    tsdn_null(tsdn)) {
		return false;
	}

	tsd_t *tsd = tsdn_tsd(tsdn);
	uint64_t n = tsd_san_extents_until_guard_small_get(tsd);
	assert(n >= 1);
	if (n == 1) {
		*tsd_san_extents_until_guard_smallp_get(tsd) =
		    opt_san_guard_small;
		return true;
	} else {
		*tsd_san_extents_until_guard_smallp_get(tsd) = n - 1;
		assert(tsd_san_extents_until_guard_small_get(tsd) >= 1);
		return false;
	}
}

#endif /* JEMALLOC_INTERNAL_GUARD_H */
