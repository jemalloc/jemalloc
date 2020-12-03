#ifndef JEMALLOC_INTERNAL_HPDATA_H
#define JEMALLOC_INTERNAL_HPDATA_H

#include "jemalloc/internal/flat_bitmap.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/typed_list.h"

/*
 * The metadata representation we use for extents in hugepages.  While the PAC
 * uses the edata_t to represent both active and inactive extents, the HP only
 * uses the edata_t for active ones; instead, inactive extent state is tracked
 * within hpdata associated with the enclosing hugepage-sized, hugepage-aligned
 * region of virtual address space.
 *
 * An hpdata need not be "truly" backed by a hugepage (which is not necessarily
 * an observable property of any given region of address space).  It's just
 * hugepage-sized and hugepage-aligned; it's *potentially* huge.
 */
typedef struct hpdata_s hpdata_t;
struct hpdata_s {
	/*
	 * We likewise follow the edata convention of mangling names and forcing
	 * the use of accessors -- this lets us add some consistency checks on
	 * access.
	 */

	/*
	 * The address of the hugepage in question.  This can't be named h_addr,
	 * since that conflicts with a macro defined in Windows headers.
	 */
	void *h_address;
	/* Its age (measured in psset operations). */
	uint64_t h_age;
	/* Whether or not we think the hugepage is mapped that way by the OS. */
	bool h_huge;
	union {
		/* When nonempty, used by the psset bins. */
		phn(hpdata_t) ph_link;
		/*
		 * When empty (or not corresponding to any hugepage), list
		 * linkage.
		 */
		ql_elm(hpdata_t) ql_link;
	};

	/* The length of the largest contiguous sequence of inactive pages. */
	size_t h_longest_free_range;

	/* Number of active pages. */
	size_t h_nactive;

	/* A bitmap with bits set in the active pages. */
	fb_group_t active_pages[FB_NGROUPS(HUGEPAGE_PAGES)];

	/*
	 * Number of dirty pages, and a bitmap tracking them.  This really means
	 * "dirty" from the OS's point of view; it includes both active and
	 * inactive pages that have been touched by the user.
	 */
	size_t h_ndirty;

	/* The dirty pages (using the same definition as above). */
	fb_group_t dirty_pages[FB_NGROUPS(HUGEPAGE_PAGES)];
};

static inline void *
hpdata_addr_get(const hpdata_t *hpdata) {
	return hpdata->h_address;
}

static inline void
hpdata_addr_set(hpdata_t *hpdata, void *addr) {
	assert(HUGEPAGE_ADDR2BASE(addr) == addr);
	hpdata->h_address = addr;
}

static inline uint64_t
hpdata_age_get(const hpdata_t *hpdata) {
	return hpdata->h_age;
}

static inline void
hpdata_age_set(hpdata_t *hpdata, uint64_t age) {
	hpdata->h_age = age;
}

static inline bool
hpdata_huge_get(const hpdata_t *hpdata) {
	return hpdata->h_huge;
}

static inline size_t
hpdata_longest_free_range_get(const hpdata_t *hpdata) {
	return hpdata->h_longest_free_range;
}

static inline void
hpdata_longest_free_range_set(hpdata_t *hpdata, size_t longest_free_range) {
	assert(longest_free_range <= HUGEPAGE_PAGES);
	hpdata->h_longest_free_range = longest_free_range;
}

static inline size_t
hpdata_nactive_get(hpdata_t *hpdata) {
	return hpdata->h_nactive;
}

static inline void
hpdata_assert_empty(hpdata_t *hpdata) {
	assert(fb_empty(hpdata->active_pages, HUGEPAGE_PAGES));
	assert(hpdata->h_nactive == 0);
}

/*
 * Only used in tests, and in hpdata_assert_consistent, below.  Verifies some
 * consistency properties of the hpdata (e.g. that cached counts of page stats
 * match computed ones).
 */
static inline bool
hpdata_consistent(hpdata_t *hpdata) {
	if(fb_urange_longest(hpdata->active_pages, HUGEPAGE_PAGES)
	    != hpdata_longest_free_range_get(hpdata)) {
		return false;
	}
	if (fb_scount(hpdata->active_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES)
	    != hpdata->h_nactive) {
		return false;
	}
	if (fb_scount(hpdata->dirty_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES)
	    != hpdata->h_ndirty) {
		return false;
	}
	if (hpdata->h_ndirty < hpdata->h_nactive) {
		return false;
	}
	if (hpdata->h_huge && hpdata->h_ndirty != HUGEPAGE_PAGES) {
		return false;
	}
	return true;
}

static inline void
hpdata_assert_consistent(hpdata_t *hpdata) {
	assert(hpdata_consistent(hpdata));
}

TYPED_LIST(hpdata_list, hpdata_t, ql_link)

typedef ph(hpdata_t) hpdata_age_heap_t;
ph_proto(, hpdata_age_heap_, hpdata_age_heap_t, hpdata_t);

static inline bool
hpdata_empty(hpdata_t *hpdata) {
	return hpdata->h_nactive == 0;
}

void hpdata_init(hpdata_t *hpdata, void *addr, uint64_t age);

/*
 * Given an hpdata which can serve an allocation request, pick and reserve an
 * offset within that allocation.
 */
void *hpdata_reserve_alloc(hpdata_t *hpdata, size_t sz);
void hpdata_unreserve(hpdata_t *hpdata, void *begin, size_t sz);

/*
 * Tell the hpdata that it's now a hugepage (which, correspondingly, means that
 * all its pages become dirty.
 */
void hpdata_hugify(hpdata_t *hpdata);
/*
 * Tell the hpdata that it's no longer a hugepage (all its pages are still
 * counted as dirty, though; an explicit purge call is required to change that).
 */
void hpdata_dehugify(hpdata_t *hpdata);
/*
 * Tell the hpdata (which should be empty) that all dirty pages in it have been
 * purged.
 */
void hpdata_purge(hpdata_t *hpdata);

#endif /* JEMALLOC_INTERNAL_HPDATA_H */
