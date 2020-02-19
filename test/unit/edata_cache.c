#include "test/jemalloc_test.h"

#include "jemalloc/internal/edata_cache.h"

static void
test_edata_cache_init(edata_cache_t *edata_cache) {
	base_t *base = base_new(TSDN_NULL, /* ind */ 1,
	    &ehooks_default_extent_hooks);
	assert_ptr_not_null(base, "");
	bool err = edata_cache_init(edata_cache, base);
	assert_false(err, "");
}

static void
test_edata_cache_destroy(edata_cache_t *edata_cache) {
	base_delete(TSDN_NULL, edata_cache->base);
}

TEST_BEGIN(test_edata_cache) {
	edata_cache_t edc;
	test_edata_cache_init(&edc);

	/* Get one */
	edata_t *ed1 = edata_cache_get(TSDN_NULL, &edc);
	assert_ptr_not_null(ed1, "");

	/* Cache should be empty */
	assert_zu_eq(atomic_load_zu(&edc.count, ATOMIC_RELAXED), 0, "");

	/* Get another */
	edata_t *ed2 = edata_cache_get(TSDN_NULL, &edc);
	assert_ptr_not_null(ed2, "");

	/* Still empty */
	assert_zu_eq(atomic_load_zu(&edc.count, ATOMIC_RELAXED), 0, "");

	/* Put one back, and the cache should now have one item */
	edata_cache_put(TSDN_NULL, &edc, ed1);
	assert_zu_eq(atomic_load_zu(&edc.count, ATOMIC_RELAXED), 1, "");

	/* Reallocating should reuse the item, and leave an empty cache. */
	edata_t *ed1_again = edata_cache_get(TSDN_NULL, &edc);
	assert_ptr_eq(ed1, ed1_again, "");
	assert_zu_eq(atomic_load_zu(&edc.count, ATOMIC_RELAXED), 0, "");

	test_edata_cache_destroy(&edc);
}
TEST_END

int
main(void) {
	return test(
	    test_edata_cache);
}
