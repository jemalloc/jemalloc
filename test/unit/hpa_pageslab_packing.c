#include "test/jemalloc_test.h"

/*
 * Tests that HPA cleanly packs allocations into a single pageslab when they
 * collectively fit, instead of growing a new one.
 */
const char *malloc_conf =
    "disable_large_size_classes:true,"
    "cache_oblivious:false,"
    "hpa_sec_nshards:0,"
    "hpa_slab_max_alloc:2097152,"
    "hpa_dirty_mult:-1,"
    "hpa_hugify_delay_ms:1000000000";

#define HPDATA_PAGES    (HUGEPAGE / PAGE)
/*
 * Every allocation must be >= SC_LARGE_MINCLASS so it goes through
 * arena_malloc_large -> pa_alloc -> hpa_alloc with the exact size we ask
 * for.  Anything <= SC_SMALL_MAXCLASS would route through the bin/slab
 * path, which sizes its own slab independent of our request.
 */
#define LARGE_MIN_PAGES (SC_LARGE_MINCLASS / PAGE)

/*
 * Sample sizes via Fibonacci numbers, filtered at runtime to fit the current
 * page geometry.  This spans a useful range (covers each pszind group, hits
 * both pszind-boundary sizes like 5/8 and non-boundary sizes like
 * 13/21/34/...) without sweeping every page count, which would balloon the
 * 3-allocation cross product.
 */
static const size_t fib_pages[] = {
	5, 8, 13, 21, 34, 55, 89, 144, 233, 377,
};
#define NFIB (sizeof(fib_pages) / sizeof(fib_pages[0]))

static bool
pages_testable(size_t pages) {
	return pages >= LARGE_MIN_PAGES && pages < HPDATA_PAGES;
}

/* Shared per-test state, populated by setup_arena. */
static int    g_alloc_flags;
static size_t g_epoch_mib[1];
static size_t g_epoch_miblen;
static size_t g_npageslabs_mib[5];
static size_t g_npageslabs_miblen;

static void
setup_arena(void) {
	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", &arena_ind, &sz, NULL, 0), 0,
	    "arenas.create failed");
	g_alloc_flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	g_epoch_miblen = sizeof(g_epoch_mib) / sizeof(g_epoch_mib[0]);
	expect_d_eq(mallctlnametomib("epoch", g_epoch_mib, &g_epoch_miblen),
	    0, "epoch mib lookup failed");

	g_npageslabs_miblen =
	    sizeof(g_npageslabs_mib) / sizeof(g_npageslabs_mib[0]);
	expect_d_eq(mallctlnametomib("stats.arenas.0.hpa_shard.npageslabs",
	    g_npageslabs_mib, &g_npageslabs_miblen), 0,
	    "npageslabs mib lookup failed");
	g_npageslabs_mib[2] = arena_ind;
}

static size_t
get_npageslabs(void) {
	uint64_t epoch = 1;
	size_t   esz   = sizeof(epoch);
	expect_d_eq(mallctlbymib(g_epoch_mib, g_epoch_miblen,
	    &epoch, &esz, &epoch, sizeof(epoch)), 0, "epoch refresh failed");
	size_t n;
	size_t nsz = sizeof(n);
	expect_d_eq(mallctlbymib(g_npageslabs_mib, g_npageslabs_miblen,
	    &n, &nsz, NULL, 0), 0, "npageslabs read failed");
	return n;
}

TEST_BEGIN(test_hpa_pageslab_packing_two_allocs) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(opt_cache_oblivious);
	test_skip_if(!sz_large_size_classes_disabled());
	setup_arena();

	for (size_t i = 0; i < NFIB; i++) {
		size_t b_pages = fib_pages[i];
		if (!pages_testable(b_pages)) {
			continue;
		}
		size_t a_pages = HPDATA_PAGES - b_pages;
		if (a_pages < LARGE_MIN_PAGES) {
			continue;
		}

		void *a = mallocx(a_pages * PAGE, g_alloc_flags);
		expect_ptr_not_null(a, "a mallocx(%zu PAGE) failed", a_pages);
		void *b = mallocx(b_pages * PAGE, g_alloc_flags);
		expect_ptr_not_null(b, "b mallocx(%zu PAGE) failed", b_pages);

		expect_zu_eq(get_npageslabs(), 1,
		    "two allocs (a=%zu b=%zu pages) should pack into one "
		    "pageslab", a_pages, b_pages);

		dallocx(b, g_alloc_flags);
		dallocx(a, g_alloc_flags);
	}
}
TEST_END

TEST_BEGIN(test_hpa_pageslab_packing_three_allocs) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(opt_cache_oblivious);
	test_skip_if(!sz_large_size_classes_disabled());
	setup_arena();

	/*
	 * Sample c (the third allocation) and a (the first allocation) from
	 * the Fibonacci table, with a up to rest - LARGE_MIN_PAGES so that
	 * b = rest - a is always at least LARGE_MIN_PAGES.  b is
	 * derived to fill the rest of the pageslab.
	 *
	 * After packing, free the middle allocation (b) first, then a, then
	 * c.  This exercises hpdata_unreserve's coalescing of free ranges
	 * with both neighbors as they appear: dealloc b leaves a single
	 * b-sized hole; dealloc a should grow that hole to a+b; dealloc c
	 * should leave the whole pageslab empty.  As a final coalescing
	 * check, allocate the entire pageslab in one call afterwards — that
	 * only succeeds if the hpdata's longest_free_range was correctly
	 * recomputed back to HPDATA_PAGES.
	 */
	for (size_t i = 0; i < NFIB; i++) {
		size_t c_pages = fib_pages[i];
		if (!pages_testable(c_pages)) {
			continue;
		}
		size_t rest = HPDATA_PAGES - c_pages;
		if (rest < 2 * LARGE_MIN_PAGES) {
			continue;
		}
		for (size_t j = 0; j < NFIB; j++) {
			size_t a_pages = fib_pages[j];
			if (!pages_testable(a_pages)) {
				continue;
			}
			if (a_pages > rest - LARGE_MIN_PAGES) {
				continue;
			}
			size_t b_pages = rest - a_pages;
			if (b_pages < LARGE_MIN_PAGES) {
				continue;
			}

			void *a = mallocx(a_pages * PAGE, g_alloc_flags);
			expect_ptr_not_null(a,
			    "a mallocx(%zu PAGE) failed", a_pages);
			void *b = mallocx(b_pages * PAGE, g_alloc_flags);
			expect_ptr_not_null(b,
			    "b mallocx(%zu PAGE) failed", b_pages);
			void *c = mallocx(c_pages * PAGE, g_alloc_flags);
			expect_ptr_not_null(c,
			    "c mallocx(%zu PAGE) failed", c_pages);

			expect_zu_eq(get_npageslabs(), 1,
			    "three allocs (a=%zu b=%zu c=%zu pages) should "
			    "pack into one pageslab",
			    a_pages, b_pages, c_pages);

			/* Free middle, then first, then last. */
			dallocx(b, g_alloc_flags);
			dallocx(a, g_alloc_flags);
			dallocx(c, g_alloc_flags);

			/*
			 * After freeing all three the hpdata should be empty
			 * with longest_free_range == HPDATA_PAGES.  The only
			 * way this allocation succeeds in one pageslab is if
			 * the coalesce on dalloc rebuilt that range.
			 */
			void *full = mallocx(HPDATA_PAGES * PAGE,
			    g_alloc_flags);
			expect_ptr_not_null(full,
			    "full mallocx(HPDATA_PAGES) after free of "
			    "(a=%zu b=%zu c=%zu) failed — coalesce broken?",
			    a_pages, b_pages, c_pages);
			expect_zu_eq(get_npageslabs(), 1,
			    "full mallocx after free of (a=%zu b=%zu c=%zu) "
			    "should fit the same pageslab",
			    a_pages, b_pages, c_pages);
			dallocx(full, g_alloc_flags);
		}
	}
}
TEST_END

int
main(void) {
	if (config_stats && hpa_supported()) {
		opt_hpa = true;
	}
	return test(
	    test_hpa_pageslab_packing_two_allocs,
	    test_hpa_pageslab_packing_three_allocs);
}
