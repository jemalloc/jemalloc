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
	edata_cache_t ec;
	test_edata_cache_init(&ec);

	/* Get one */
	edata_t *ed1 = edata_cache_get(TSDN_NULL, &ec);
	assert_ptr_not_null(ed1, "");

	/* Cache should be empty */
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	/* Get another */
	edata_t *ed2 = edata_cache_get(TSDN_NULL, &ec);
	assert_ptr_not_null(ed2, "");

	/* Still empty */
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	/* Put one back, and the cache should now have one item */
	edata_cache_put(TSDN_NULL, &ec, ed1);
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 1, "");

	/* Reallocating should reuse the item, and leave an empty cache. */
	edata_t *ed1_again = edata_cache_get(TSDN_NULL, &ec);
	assert_ptr_eq(ed1, ed1_again, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	test_edata_cache_destroy(&ec);
}
TEST_END

TEST_BEGIN(test_edata_cache_small) {
	edata_cache_t ec;
	edata_cache_small_t ecs;

	test_edata_cache_init(&ec);
	edata_cache_small_init(&ecs, &ec);

	bool err = edata_cache_small_prepare(TSDN_NULL, &ecs, 2);
	assert_false(err, "");
	assert_zu_eq(ecs.count, 2, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	edata_t *ed1 = edata_cache_small_get(&ecs);
	assert_zu_eq(ecs.count, 1, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	edata_t *ed2 = edata_cache_small_get(&ecs);
	assert_zu_eq(ecs.count, 0, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	edata_cache_small_put(&ecs, ed1);
	assert_zu_eq(ecs.count, 1, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	edata_cache_small_put(&ecs, ed2);
	assert_zu_eq(ecs.count, 2, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 0, "");

	edata_cache_small_finish(TSDN_NULL, &ecs, 1);
	assert_zu_eq(ecs.count, 1, "");
	assert_zu_eq(atomic_load_zu(&ec.count, ATOMIC_RELAXED), 1, "");

	test_edata_cache_destroy(&ec);
}
TEST_END

int
main(void) {
	return test(
	    test_edata_cache,
	    test_edata_cache_small);
}
