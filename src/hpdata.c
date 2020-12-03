#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpdata.h"

static int
hpdata_age_comp(const hpdata_t *a, const hpdata_t *b) {
	uint64_t a_age = hpdata_age_get(a);
	uint64_t b_age = hpdata_age_get(b);
	/*
	 * hpdata ages are operation counts in the psset; no two should be the
	 * same.
	 */
	assert(a_age != b_age);
	return (a_age > b_age) - (a_age < b_age);
}

ph_gen(, hpdata_age_heap_, hpdata_age_heap_t, hpdata_t, ph_link, hpdata_age_comp)

void
hpdata_init(hpdata_t *hpdata, void *addr, uint64_t age) {
	hpdata_addr_set(hpdata, addr);
	hpdata_age_set(hpdata, age);
	hpdata_huge_set(hpdata, false);
	hpdata->h_nactive = 0;
	hpdata_longest_free_range_set(hpdata, HUGEPAGE_PAGES);
	fb_init(hpdata->active_pages, HUGEPAGE_PAGES);

	hpdata_assert_consistent(hpdata);
}

void *
hpdata_reserve_alloc(hpdata_t *hpdata, size_t sz) {
	hpdata_assert_consistent(hpdata);
	assert((sz & PAGE_MASK) == 0);
	size_t npages = sz >> LG_PAGE;
	assert(npages <= hpdata_longest_free_range_get(hpdata));

	size_t result;

	size_t start = 0;
	/*
	 * These are dead stores, but the compiler will issue warnings on them
	 * since it can't tell statically that found is always true below.
	 */
	size_t begin = 0;
	size_t len = 0;

	size_t largest_unchosen_range = 0;
	while (true) {
		bool found = fb_urange_iter(hpdata->active_pages,
		    HUGEPAGE_PAGES, start, &begin, &len);
		/*
		 * A precondition to this function is that hpdata must be able
		 * to serve the allocation.
		 */
		assert(found);
		if (len >= npages) {
			/*
			 * We use first-fit within the page slabs; this gives
			 * bounded worst-case fragmentation within a slab.  It's
			 * not necessarily right; we could experiment with
			 * various other options.
			 */
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	/* We found a range; remember it. */
	result = begin;
	fb_set_range(hpdata->active_pages, HUGEPAGE_PAGES, begin, npages);
	hpdata->h_nactive += npages;

	/*
	 * We might have shrunk the longest free range.  We have to keep
	 * scanning until the end of the hpdata to be sure.
	 *
	 * TODO: As an optimization, we should only do this when the range we
	 * just allocated from was equal to the longest free range size.
	 */
	start = begin + npages;
	while (start < HUGEPAGE_PAGES) {
		bool found = fb_urange_iter(hpdata->active_pages,
		    HUGEPAGE_PAGES, start, &begin, &len);
		if (!found) {
			break;
		}
		if (len > largest_unchosen_range) {
			largest_unchosen_range = len;
		}
		start = begin + len;
	}
	hpdata_longest_free_range_set(hpdata, largest_unchosen_range);

	hpdata_assert_consistent(hpdata);
	return (void *)(
	    (uintptr_t)hpdata_addr_get(hpdata) + (result << LG_PAGE));
}

void
hpdata_unreserve(hpdata_t *hpdata, void *addr, size_t sz) {
	hpdata_assert_consistent(hpdata);
	assert(((uintptr_t)addr & PAGE_MASK) == 0);
	assert((sz & PAGE_MASK) == 0);
	size_t begin = ((uintptr_t)addr - (uintptr_t)hpdata_addr_get(hpdata))
	    >> LG_PAGE;
	assert(begin < HUGEPAGE_PAGES);
	size_t npages = sz >> LG_PAGE;
	size_t old_longest_range = hpdata_longest_free_range_get(hpdata);

	fb_unset_range(hpdata->active_pages, HUGEPAGE_PAGES, begin, npages);
	/* We might have just created a new, larger range. */
	size_t new_begin = (fb_fls(hpdata->active_pages, HUGEPAGE_PAGES,
	    begin) + 1);
	size_t new_end = fb_ffs(hpdata->active_pages, HUGEPAGE_PAGES,
	    begin + npages - 1);
	size_t new_range_len = new_end - new_begin;

	if (new_range_len > old_longest_range) {
		hpdata_longest_free_range_set(hpdata, new_range_len);
	}

	hpdata->h_nactive -= npages;

	hpdata_assert_consistent(hpdata);
}
