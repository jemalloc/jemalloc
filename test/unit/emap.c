#include "test/jemalloc_test.h"

#include "jemalloc/internal/emap.h"

#define EMAP_TEST_ARENA_IND 123
#define EMAP_TEST_ADDR_BASE ((uintptr_t)0x40000000U)

typedef struct emap_test_data_s emap_test_data_t;
struct emap_test_data_s {
	base_t *base;
	emap_t emap;
	malloc_mutex_t mtx;
};

static edata_t *
test_edata_alloc(uintptr_t addr, size_t size, bool slab, szind_t szind,
    uint64_t sn, extent_state_t state, extent_pai_t pai,
    extent_head_state_t is_head) {
	edata_t *edata = (edata_t *)mallocx(sizeof(edata_t),
	    MALLOCX_ALIGN(EDATA_ALIGNMENT));
	assert_ptr_not_null(edata, "Unexpected edata allocation failure");
	memset(edata, 0, sizeof(*edata));
	edata_init(edata, EMAP_TEST_ARENA_IND, (void *)addr, size, slab,
	    szind, sn, state, /* zeroed */ false, /* committed */ true, pai,
	    is_head);
	return edata;
}

static emap_test_data_t *
emap_test_data_create(void) {
	emap_test_data_t *data = calloc(1, sizeof(*data));
	assert_ptr_not_null(data, "Unexpected calloc failure");
	data->base = base_new(TSDN_NULL, EMAP_TEST_ARENA_IND,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(data->base, "Unexpected base_new failure");
	assert_false(emap_init(&data->emap, data->base, /* zeroed */ true),
	    "Unexpected emap_init failure");
	assert_false(malloc_mutex_init(&data->mtx, "emap_test",
	    WITNESS_RANK_EXTENTS, malloc_mutex_rank_exclusive),
	    "Unexpected mutex initialization failure");
	return data;
}

static void
emap_test_data_destroy(emap_test_data_t *data) {
	base_delete(TSDN_NULL, data->base);
	free(data);
}

static void
expect_full_lookup(emap_t *emap, void *ptr, edata_t *edata, szind_t szind,
    bool slab) {
	emap_full_alloc_ctx_t ctx;
	emap_full_alloc_ctx_lookup(TSDN_NULL, emap, ptr, &ctx);
	expect_ptr_eq(edata, ctx.edata, "Unexpected emap edata");
	expect_u_eq(szind, ctx.szind, "Unexpected emap szind");
	expect_b_eq(slab, ctx.slab, "Unexpected emap slab bit");
}

TEST_BEGIN(test_emap_register_and_lookup_slab) {
	emap_test_data_t *data = emap_test_data_create();

	szind_t szind = 0;
	edata_t *slab = test_edata_alloc(EMAP_TEST_ADDR_BASE, 4 * PAGE,
	    /* slab */ true, szind, 1, extent_state_active, EXTENT_PAI_PAC,
	    EXTENT_NOT_HEAD);
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap, slab,
	    szind, /* slab */ true),
	    "Unexpected boundary registration failure");
	emap_register_interior(TSDN_NULL, &data->emap, slab, szind);
	expect_full_lookup(&data->emap, (void *)EMAP_TEST_ADDR_BASE, slab,
	    szind, true);
	expect_full_lookup(&data->emap, (void *)(EMAP_TEST_ADDR_BASE + PAGE),
	    slab, szind, true);
	expect_full_lookup(&data->emap,
	    (void *)(EMAP_TEST_ADDR_BASE + 3 * PAGE), slab, szind, true);

	emap_deregister_interior(TSDN_NULL, &data->emap, slab);
	emap_deregister_boundary(TSDN_NULL, &data->emap, slab);
	expect_ptr_null(emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)EMAP_TEST_ADDR_BASE), "Boundary should be cleared");

	dallocx(slab, 0);
	emap_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_emap_remap_updates_szind) {
	emap_test_data_t *data = emap_test_data_create();

	szind_t szind = 0;
	edata_t *remap = test_edata_alloc(EMAP_TEST_ADDR_BASE + HUGEPAGE,
	    2 * PAGE, /* slab */ false, SC_NSIZES, 2, extent_state_active,
	    EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap, remap,
	    SC_NSIZES, /* slab */ false),
	    "Unexpected boundary registration failure");
	emap_remap(TSDN_NULL, &data->emap, remap, szind, /* slab */ false);
	expect_full_lookup(&data->emap, edata_base_get(remap), remap, szind,
	    false);
	expect_full_lookup(&data->emap, edata_last_get(remap), remap,
	    SC_NSIZES, false);

	dallocx(remap, 0);
	emap_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_emap_split_then_merge) {
	emap_test_data_t *data = emap_test_data_create();

	uintptr_t split_base = EMAP_TEST_ADDR_BASE + 2 * HUGEPAGE;
	edata_t *lead = test_edata_alloc(split_base, 4 * PAGE,
	    /* slab */ false, SC_NSIZES, 3, extent_state_active,
	    EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	edata_t *trail = test_edata_alloc(split_base + 2 * PAGE, 2 * PAGE,
	    /* slab */ false, SC_NSIZES, 3, extent_state_active,
	    EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap, lead,
	    SC_NSIZES, /* slab */ false),
	    "Unexpected boundary registration failure");

	emap_prepare_t prepare;
	expect_false(emap_split_prepare(TSDN_NULL, &data->emap, &prepare, lead,
	    2 * PAGE, trail, 2 * PAGE), "Unexpected split prepare failure");
	edata_size_set(lead, 2 * PAGE);
	emap_split_commit(TSDN_NULL, &data->emap, &prepare, lead, 2 * PAGE,
	    trail, 2 * PAGE);
	expect_ptr_eq(lead, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)split_base), "Split lead base should map to lead");
	expect_ptr_eq(lead, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + PAGE)), "Split lead end should map to lead");
	expect_ptr_eq(trail, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + 2 * PAGE)),
	    "Split trail base should map to trail");
	expect_ptr_eq(trail, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + 3 * PAGE)),
	    "Split trail end should map to trail");

	emap_merge_prepare(TSDN_NULL, &data->emap, &prepare, lead, trail);
	edata_size_set(lead, 4 * PAGE);
	emap_merge_commit(TSDN_NULL, &data->emap, &prepare, lead, trail);
	expect_ptr_eq(lead, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)split_base), "Merged base should map to lead");
	expect_ptr_null(emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + PAGE)),
	    "Old lead boundary should be cleared after merge");
	expect_ptr_null(emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + 2 * PAGE)),
	    "Old trail boundary should be cleared after merge");
	expect_ptr_eq(lead, emap_edata_lookup(TSDN_NULL, &data->emap,
	    (void *)(split_base + 3 * PAGE)),
	    "Merged last page should map to lead");

	dallocx(lead, 0);
	dallocx(trail, 0);
	emap_test_data_destroy(data);
}
TEST_END

TEST_BEGIN(test_emap_neighbor_acquisition) {
	emap_test_data_t *data = emap_test_data_create();

	edata_t *page_one = test_edata_alloc(PAGE, PAGE, /* slab */ false,
	    SC_NSIZES, 1, extent_state_active, EXTENT_PAI_PAC,
	    EXTENT_NOT_HEAD);
	malloc_mutex_lock(TSDN_NULL, &data->mtx);
	expect_ptr_null(emap_try_acquire_edata_neighbor(TSDN_NULL, &data->emap,
	    page_one, EXTENT_PAI_PAC, extent_state_dirty,
	    /* forward */ false),
	    "Backward acquisition from the first page should return NULL");
	malloc_mutex_unlock(TSDN_NULL, &data->mtx);

	uintptr_t base = EMAP_TEST_ADDR_BASE + 4 * HUGEPAGE;
	edata_t *active = test_edata_alloc(base, PAGE, /* slab */ false,
	    SC_NSIZES, 2, extent_state_active, EXTENT_PAI_PAC,
	    EXTENT_NOT_HEAD);
	edata_t *dirty = test_edata_alloc(base + PAGE, PAGE,
	    /* slab */ false, SC_NSIZES, 3, extent_state_active,
	    EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap, active,
	    SC_NSIZES, /* slab */ false), "Unexpected registration failure");
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap, dirty,
	    SC_NSIZES, /* slab */ false), "Unexpected registration failure");

	malloc_mutex_lock(TSDN_NULL, &data->mtx);
	emap_update_edata_state(TSDN_NULL, &data->emap, dirty,
	    extent_state_dirty);
	expect_ptr_null(emap_try_acquire_edata_neighbor(TSDN_NULL, &data->emap,
	    active, EXTENT_PAI_PAC, extent_state_muzzy, /* forward */ true),
	    "State mismatch should reject neighbor acquisition");
	expect_d_eq(extent_state_dirty, edata_state_get(dirty),
	    "Rejected neighbor should keep its state");

	edata_t *neighbor = emap_try_acquire_edata_neighbor(TSDN_NULL,
	    &data->emap, active, EXTENT_PAI_PAC, extent_state_dirty,
	    /* forward */ true);
	expect_ptr_eq(dirty, neighbor, "Expected forward dirty neighbor");
	expect_d_eq(extent_state_merging, edata_state_get(dirty),
	    "Acquired neighbor should enter merging state");
	emap_release_edata(TSDN_NULL, &data->emap, dirty, extent_state_dirty);
	expect_d_eq(extent_state_dirty, edata_state_get(dirty),
	    "Released neighbor should return to requested state");
	malloc_mutex_unlock(TSDN_NULL, &data->mtx);

	edata_t *hpa_neighbor = test_edata_alloc(base + 2 * PAGE, PAGE,
	    /* slab */ false, SC_NSIZES, 4, extent_state_active,
	    EXTENT_PAI_HPA, EXTENT_NOT_HEAD);
	expect_false(emap_register_boundary(TSDN_NULL, &data->emap,
	    hpa_neighbor, SC_NSIZES, /* slab */ false),
	    "Unexpected registration failure");
	malloc_mutex_lock(TSDN_NULL, &data->mtx);
	emap_update_edata_state(
	    TSDN_NULL, &data->emap, hpa_neighbor, extent_state_dirty);
	expect_ptr_null(emap_try_acquire_edata_neighbor_expand(TSDN_NULL,
	    &data->emap, dirty, EXTENT_PAI_PAC, extent_state_dirty),
	    "PAI mismatch should reject expand acquisition");
	emap_update_edata_state(TSDN_NULL, &data->emap, hpa_neighbor,
	    extent_state_active);
	malloc_mutex_unlock(TSDN_NULL, &data->mtx);

	dallocx(page_one, 0);
	dallocx(active, 0);
	dallocx(dirty, 0);
	dallocx(hpa_neighbor, 0);
	emap_test_data_destroy(data);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_emap_register_and_lookup_slab,
	    test_emap_remap_updates_szind,
	    test_emap_split_then_merge,
	    test_emap_neighbor_acquisition);
}
