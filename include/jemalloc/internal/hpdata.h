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

	/*
	 * Whether or not some thread is purging this hpdata (i.e. has called
	 * hpdata_purge_begin but not yet called hpdata_purge_end), or
	 * hugifying it.  Only one thread at a time is allowed to change a
	 * hugepage's state.
	 */
	bool h_mid_purge;
	bool h_mid_hugify;

	/* Whether or not the hpdata is a the psset. */
	bool h_in_psset;

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
	 * Number of dirty or active pages, and a bitmap tracking them.  One
	 * way to think of this is as which pages are dirty from the OS's
	 * perspective.
	 */
	size_t h_ntouched;

	/* The dirty pages (using the same definition as above). */
	fb_group_t touched_pages[FB_NGROUPS(HUGEPAGE_PAGES)];
};

TYPED_LIST(hpdata_list, hpdata_t, ql_link)
typedef ph(hpdata_t) hpdata_age_heap_t;
ph_proto(, hpdata_age_heap_, hpdata_age_heap_t, hpdata_t);

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

static inline bool
hpdata_changing_state_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_purge || hpdata->h_mid_hugify;
}

static inline bool
hpdata_mid_purge_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_purge;
}

static inline bool
hpdata_mid_hugify_get(const hpdata_t *hpdata) {
	return hpdata->h_mid_hugify;
}

static inline bool
hpdata_in_psset_get(const hpdata_t *hpdata) {
	return hpdata->h_in_psset;
}

static inline void
hpdata_in_psset_set(hpdata_t *hpdata, bool in_psset) {
	hpdata->h_in_psset = in_psset;
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

static inline size_t
hpdata_ntouched_get(hpdata_t *hpdata) {
	return hpdata->h_ntouched;
}

static inline size_t
hpdata_ndirty_get(hpdata_t *hpdata) {
	return hpdata->h_ntouched - hpdata->h_nactive;
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
	if (fb_scount(hpdata->touched_pages, HUGEPAGE_PAGES, 0, HUGEPAGE_PAGES)
	    != hpdata->h_ntouched) {
		return false;
	}
	if (hpdata->h_ntouched < hpdata->h_nactive) {
		return false;
	}
	if (hpdata->h_huge && hpdata->h_ntouched != HUGEPAGE_PAGES) {
		return false;
	}
	return true;
}

static inline void
hpdata_assert_consistent(hpdata_t *hpdata) {
	assert(hpdata_consistent(hpdata));
}

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
 * The hpdata_purge_prepare_t allows grabbing the metadata required to purge
 * subranges of a hugepage while holding a lock, drop the lock during the actual
 * purging of them, and reacquire it to update the metadata again.
 */
typedef struct hpdata_purge_state_s hpdata_purge_state_t;
struct hpdata_purge_state_s {
	size_t npurged;
	fb_group_t to_purge[FB_NGROUPS(HUGEPAGE_PAGES)];
	size_t next_purge_search_begin;
};

/*
 * Initializes purge state.  The access to hpdata must be externally
 * synchronized with other hpdata_* calls.
 *
 * You can tell whether or not a thread is purging or hugifying a given hpdata
 * via hpdata_changing_state_get(hpdata).  Racing hugification or purging
 * operations aren't allowed.
 *
 * Once you begin purging, you have to follow through and call hpdata_purge_next
 * until you're done, and then end.  Allocating out of an hpdata undergoing
 * purging is not allowed.
 */
void hpdata_purge_begin(hpdata_t *hpdata, hpdata_purge_state_t *purge_state);
/*
 * If there are more extents to purge, sets *r_purge_addr and *r_purge_size to
 * true, and returns true.  Otherwise, returns false to indicate that we're
 * done.
 *
 * This requires exclusive access to the purge state, but *not* to the hpdata.
 * In particular, unreserve calls are allowed while purging (i.e. you can dalloc
 * into one part of the hpdata while purging a different part).
 */
bool hpdata_purge_next(hpdata_t *hpdata, hpdata_purge_state_t *purge_state,
    void **r_purge_addr, size_t *r_purge_size);
/*
 * Updates the hpdata metadata after all purging is done.  Needs external
 * synchronization.
 */
void hpdata_purge_end(hpdata_t *hpdata, hpdata_purge_state_t *purge_state);

/*
 * Similarly, when hugifying , callers can do the metadata modifications while
 * holding a lock (thereby setting the change_state field), but actually do the
 * operation without blocking other threads.
 *
 * Unlike most metadata operations, hugification ending should happen while an
 * hpdata is in the psset (or upcoming hugepage collections).  This is because
 * while purge/use races are unsafe, purge/hugepageify races are perfectly
 * reasonable.
 */
void hpdata_hugify_begin(hpdata_t *hpdata);
void hpdata_hugify_end(hpdata_t *hpdata);

/*
 * Tell the hpdata that it's no longer a hugepage (all its pages are still
 * counted as dirty, though; an explicit purge call is required to change that).
 *
 * This should only be done after starting to purge, and before actually purging
 * any contents.
 */
void hpdata_dehugify(hpdata_t *hpdata);

#endif /* JEMALLOC_INTERNAL_HPDATA_H */
