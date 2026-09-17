#include "test/jemalloc_test.h"

#include "test/hpa.h"

#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/hpa_central.h"

#define HPA_TEST_ARENA_IND 125
#define HPA_TEST_EDEN_SIZE (128 * HUGEPAGE)

static bool hpa_test_map_fail;
static unsigned hpa_test_map_calls;
static unsigned hpa_test_unmap_calls;
static unsigned hpa_test_hugify_calls;
static void *hpa_test_last_map;
static size_t hpa_test_last_map_size;
static void *hpa_test_last_unmap;
static size_t hpa_test_last_unmap_size;
static void *hpa_test_last_hugify;
static size_t hpa_test_last_hugify_size;

static void
hpa_test_hooks_reset(void) {
	hpa_test_map_fail = false;
	hpa_test_map_calls = 0;
	hpa_test_unmap_calls = 0;
	hpa_test_hugify_calls = 0;
	hpa_test_last_map = NULL;
	hpa_test_last_map_size = 0;
	hpa_test_last_unmap = NULL;
	hpa_test_last_unmap_size = 0;
	hpa_test_last_hugify = NULL;
	hpa_test_last_hugify_size = 0;
}

static void *
hpa_test_map(size_t size) {
	hpa_test_map_calls++;
	hpa_test_last_map_size = size;
	if (hpa_test_map_fail) {
		hpa_test_last_map = NULL;
		return NULL;
	}
	bool commit = true;
	void *ret = pages_map(NULL, size, HUGEPAGE, &commit);
	assert_true(commit, "HPA test mappings should be committed");
	hpa_test_last_map = ret;
	return ret;
}

static void
hpa_test_unmap(void *ptr, size_t size) {
	hpa_test_unmap_calls++;
	hpa_test_last_unmap = ptr;
	hpa_test_last_unmap_size = size;
	pages_unmap(ptr, size);
}

static void
hpa_test_purge(void *ptr, size_t size) {
}

static bool
hpa_test_hugify(void *ptr, size_t size, bool sync) {
	hpa_test_hugify_calls++;
	hpa_test_last_hugify = ptr;
	hpa_test_last_hugify_size = size;
	return false;
}

static void
hpa_test_dehugify(void *ptr, size_t size) {
}

static void
hpa_test_curtime(nstime_t *r_time, bool first_reading) {
	nstime_init(r_time, 0);
}

static uint64_t
hpa_test_ms_since(nstime_t *r_time) {
	return 0;
}

static bool
hpa_test_vectorized_purge(void *vec, size_t vlen, size_t nbytes) {
	return true;
}

static hpa_hooks_t hpa_test_hooks = {
	hpa_test_map,
	hpa_test_unmap,
	hpa_test_purge,
	hpa_test_hugify,
	hpa_test_dehugify,
	hpa_test_curtime,
	hpa_test_ms_since,
	hpa_test_vectorized_purge
};

static bool hpa_base_fail_after_new;
static unsigned hpa_base_alloc_calls;

static void
hpa_base_hooks_reset(void) {
	hpa_base_fail_after_new = false;
	hpa_base_alloc_calls = 0;
}

static void *
hpa_base_alloc(extent_hooks_t *extent_hooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	if (hpa_base_fail_after_new && hpa_base_alloc_calls > 0) {
		hpa_base_alloc_calls++;
		return NULL;
	}
	hpa_base_alloc_calls++;
	return pages_map(new_addr, size, alignment, commit);
}

static bool
hpa_base_dalloc(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	pages_unmap(addr, size);
	return false;
}

static void
hpa_base_destroy(extent_hooks_t *extent_hooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	pages_unmap(addr, size);
}

static extent_hooks_t hpa_base_hooks = {
	hpa_base_alloc,
	hpa_base_dalloc,
	hpa_base_destroy,
	NULL, /* commit */
	NULL, /* decommit */
	NULL, /* purge_lazy */
	NULL, /* purge_forced */
	NULL, /* split */
	NULL  /* merge */
};

static hpdata_t *
hpa_central_extract_with_lock(hpa_central_t *central, malloc_mutex_t *mtx,
    uint64_t age, bool hugify_eager, bool *oom) {
	tsdn_t *tsdn = tsdn_fetch();
	malloc_mutex_lock(tsdn, mtx);
	hpdata_t *ret = hpa_central_extract(tsdn, central, PAGE, age,
	    hugify_eager, oom);
	malloc_mutex_unlock(tsdn, mtx);
	return ret;
}

static void
hpa_test_shard_grow_mtx_init(malloc_mutex_t *mtx) {
	assert_false(malloc_mutex_init(mtx, "hpa_test_shard_grow",
	    WITNESS_RANK_HPA_SHARD_GROW, malloc_mutex_rank_exclusive),
	    "Unexpected mutex initialization failure");
}

TEST_BEGIN(test_hpa_central_extract_eden) {
	test_skip_if(!hpa_supported() || hpa_hugepage_size_exceeds_limit());
	hpa_test_hooks_reset();

	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, HPA_TEST_ARENA_IND,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "Unexpected base_new failure");

	hpa_central_t central;
	assert_false(hpa_central_init(&central, base, &hpa_test_hooks),
	    "Unexpected hpa_central_init failure");
	malloc_mutex_t shard_grow_mtx;
	hpa_test_shard_grow_mtx_init(&shard_grow_mtx);

	void *eden = NULL;
	for (unsigned i = 0; i < 128; i++) {
		bool oom = true;
		hpdata_t *ps = hpa_central_extract_with_lock(&central,
		    &shard_grow_mtx, 1000 + i, /* hugify_eager */ true, &oom);
		expect_false(oom, "Unexpected HPA central OOM");
		expect_ptr_not_null(ps, "Unexpected HPA central extraction failure");
		if (i == 0) {
			eden = hpa_test_last_map;
			expect_u_eq(1, hpa_test_map_calls,
			    "First extraction should map eden");
			expect_zu_eq(HPA_TEST_EDEN_SIZE, hpa_test_last_map_size,
			    "Unexpected eden mapping size");
			expect_u_eq(1, hpa_test_hugify_calls,
			    "Eager extraction should hugify the whole eden");
			expect_ptr_eq(eden, hpa_test_last_hugify,
			    "Hugify should apply to eden");
			expect_zu_eq(HPA_TEST_EDEN_SIZE,
			    hpa_test_last_hugify_size,
			    "Hugify should cover the whole eden");
		}
		expect_ptr_eq((void *)((byte_t *)eden + i * HUGEPAGE),
		    hpdata_addr_get(ps), "Unexpected extracted pageslab addr");
		expect_u64_eq(1000 + i, hpdata_age_get(ps),
		    "Unexpected hpdata age");
		expect_true(hpdata_huge_get(ps),
		    "Eager extraction should mark hpdata huge");
	}

	expect_ptr_null(central.eden, "Exact final extraction should empty eden");
	expect_zu_eq(0, central.eden_len,
	    "Exact final extraction should clear eden length");
	expect_u_eq(1, hpa_test_map_calls,
	    "All pageslabs should come from one eden mapping");
	expect_u_eq(0, hpa_test_unmap_calls,
	    "Successful extraction should not unmap eden");

	pages_unmap(eden, HPA_TEST_EDEN_SIZE);
	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_hpa_central_failure_paths) {
	test_skip_if(!hpa_supported() || hpa_hugepage_size_exceeds_limit());
	hpa_test_hooks_reset();

	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, HPA_TEST_ARENA_IND + 1,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "Unexpected base_new failure");
	hpa_central_t central;
	assert_false(hpa_central_init(&central, base, &hpa_test_hooks),
	    "Unexpected hpa_central_init failure");
	malloc_mutex_t shard_grow_mtx;
	hpa_test_shard_grow_mtx_init(&shard_grow_mtx);

	hpa_test_map_fail = true;
	bool oom = false;
	hpdata_t *ps = hpa_central_extract_with_lock(&central,
	    &shard_grow_mtx, 1, /* hugify_eager */ false, &oom);
	expect_ptr_null(ps, "Map failure should not return hpdata");
	expect_true(oom, "Map failure should report OOM");
	expect_u_eq(1, hpa_test_map_calls, "Expected one map attempt");
	expect_u_eq(0, hpa_test_unmap_calls,
	    "Map failure should not call unmap");
	base_delete(tsdn, base);

	hpa_base_hooks_reset();
	hpa_test_hooks_reset();
	base = base_new(tsdn, HPA_TEST_ARENA_IND + 2, &hpa_base_hooks,
	    /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "Unexpected base_new failure");
	hpa_base_fail_after_new = true;
	while (base_alloc(tsdn, base, sizeof(hpdata_t), CACHELINE) != NULL) {
	}

	assert_false(hpa_central_init(&central, base, &hpa_test_hooks),
	    "Unexpected hpa_central_init failure");
	malloc_mutex_t shard_grow_mtx2;
	hpa_test_shard_grow_mtx_init(&shard_grow_mtx2);
	oom = false;
	ps = hpa_central_extract_with_lock(&central, &shard_grow_mtx2, 2,
	    /* hugify_eager */ false, &oom);
	expect_ptr_null(ps, "Metadata OOM should not return hpdata");
	expect_true(oom, "Metadata allocation failure should report OOM");
	expect_u_eq(1, hpa_test_map_calls,
	    "Metadata OOM should happen after mapping eden once");
	expect_u_eq(1, hpa_test_unmap_calls,
	    "Metadata OOM should unmap the freshly mapped eden");
	expect_ptr_eq(hpa_test_last_map, hpa_test_last_unmap,
	    "Metadata OOM should unmap the eden it just mapped");
	expect_zu_eq(HPA_TEST_EDEN_SIZE, hpa_test_last_unmap_size,
	    "Metadata OOM should unmap the full eden");
	base_delete(tsdn, base);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_hpa_central_extract_eden,
	    test_hpa_central_failure_paths);
}
