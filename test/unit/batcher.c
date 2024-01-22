#include "test/jemalloc_test.h"

#include "jemalloc/internal/batcher.h"

TEST_BEGIN(test_simple) {
	enum { NELEMS_MAX = 10, DATA_BASE_VAL = 100, NRUNS = 5 };
	batcher_t batcher;
	size_t data[NELEMS_MAX];
	for (size_t nelems = 0; nelems < NELEMS_MAX; nelems++) {
		batcher_init(&batcher, nelems);
		for (int run = 0; run < NRUNS; run++) {
			for (int i = 0; i < NELEMS_MAX; i++) {
				data[i] = (size_t)-1;
			}
			for (size_t i = 0; i < nelems; i++) {
				size_t idx = batcher_push_begin(TSDN_NULL,
				    &batcher, 1);
				assert_zu_eq(i, idx, "Wrong index");
				assert_zu_eq((size_t)-1, data[idx],
				    "Expected uninitialized slot");
				data[idx] = DATA_BASE_VAL + i;
				batcher_push_end(TSDN_NULL, &batcher);
			}
			if (nelems > 0) {
				size_t idx = batcher_push_begin(TSDN_NULL,
				    &batcher, 1);
				assert_zu_eq(BATCHER_NO_IDX, idx,
				    "Shouldn't be able to push into a full "
				    "batcher");
			}

			size_t npop = batcher_pop_begin(TSDN_NULL, &batcher);
			if (nelems == 0) {
				assert_zu_eq(npop, BATCHER_NO_IDX,
				    "Shouldn't get any items out of an empty "
				    "batcher");
			} else {
				assert_zu_eq(npop, nelems,
				    "Wrong number of elements popped");
			}
			for (size_t i = 0; i < nelems; i++) {
				assert_zu_eq(data[i], DATA_BASE_VAL + i,
				    "Item popped out of order!");
			}
			if (nelems != 0) {
				batcher_pop_end(TSDN_NULL, &batcher);
			}
		}
	}
}
TEST_END

TEST_BEGIN(test_multi_push) {
	size_t idx, nelems;
	batcher_t batcher;
	batcher_init(&batcher, 11);
	/* Push two at a time, 5 times, for 10 total. */
	for (int i = 0; i < 5; i++) {
		idx = batcher_push_begin(TSDN_NULL, &batcher, 2);
		assert_zu_eq(2 * i, idx, "Should push in order");
		batcher_push_end(TSDN_NULL, &batcher);
	}
	/* Pushing two more should fail -- would put us at 12 elems. */
	idx = batcher_push_begin(TSDN_NULL, &batcher, 2);
	assert_zu_eq(BATCHER_NO_IDX, idx, "Should be out of space");
	/* But one more should work */
	idx = batcher_push_begin(TSDN_NULL, &batcher, 1);
	assert_zu_eq(10, idx, "Should be out of space");
	batcher_push_end(TSDN_NULL, &batcher);
	nelems = batcher_pop_begin(TSDN_NULL, &batcher);
	batcher_pop_end(TSDN_NULL, &batcher);
	assert_zu_eq(11, nelems, "Should have popped everything");
}
TEST_END

enum {
	STRESS_TEST_ELEMS = 10,
	STRESS_TEST_THREADS = 4,
	STRESS_TEST_OPS = 1000 * 1000,
	STRESS_TEST_PUSH_TO_POP_RATIO = 5,
};

typedef struct stress_test_data_s stress_test_data_t;
struct stress_test_data_s {
	batcher_t batcher;
	mtx_t pop_mtx;
	atomic_u32_t thread_id;

	uint32_t elems_data[STRESS_TEST_ELEMS];
	size_t push_count[STRESS_TEST_ELEMS];
	size_t pop_count[STRESS_TEST_ELEMS];
	atomic_zu_t atomic_push_count[STRESS_TEST_ELEMS];
	atomic_zu_t atomic_pop_count[STRESS_TEST_ELEMS];
};

/*
 * Note: 0-indexed. If one element is set and you want to find it, you call
 * get_nth_set(elems, 0).
 */
static size_t
get_nth_set(bool elems_owned[STRESS_TEST_ELEMS], size_t n) {
	size_t ntrue = 0;
	for (size_t i = 0; i < STRESS_TEST_ELEMS; i++) {
		if (elems_owned[i]) {
			ntrue++;
		}
		if (ntrue > n) {
			return i;
		}
	}
	assert_not_reached("Asked for the %zu'th set element when < %zu are "
	    "set",
	    n, n);
	/* Just to silence a compiler warning. */
	return 0;
}

static void *
stress_test_thd(void *arg) {
	stress_test_data_t *data = arg;
	size_t prng = atomic_fetch_add_u32(&data->thread_id, 1,
	    ATOMIC_RELAXED);

	size_t nelems_owned = 0;
	bool elems_owned[STRESS_TEST_ELEMS] = {0};
	size_t local_push_count[STRESS_TEST_ELEMS] = {0};
	size_t local_pop_count[STRESS_TEST_ELEMS] = {0};

	for (int i = 0; i < STRESS_TEST_OPS; i++) {
		size_t rnd = prng_range_zu(&prng,
		    STRESS_TEST_PUSH_TO_POP_RATIO);
		if (rnd == 0 || nelems_owned == 0) {
			size_t nelems = batcher_pop_begin(TSDN_NULL,
			    &data->batcher);
			if (nelems == BATCHER_NO_IDX) {
				continue;
			}
			for (size_t i = 0; i < nelems; i++) {
				uint32_t elem = data->elems_data[i];
				assert_false(elems_owned[elem],
				    "Shouldn't already own what we just "
				    "popped");
				elems_owned[elem] = true;
				nelems_owned++;
				local_pop_count[elem]++;
				data->pop_count[elem]++;
			}
			batcher_pop_end(TSDN_NULL, &data->batcher);
		} else {
			size_t elem_to_push_idx = prng_range_zu(&prng,
			    nelems_owned);
			size_t elem = get_nth_set(elems_owned,
			    elem_to_push_idx);
			assert_true(
			    elems_owned[elem],
			    "Should own element we're about to pop");
			elems_owned[elem] = false;
			local_push_count[elem]++;
			data->push_count[elem]++;
			nelems_owned--;
			size_t idx = batcher_push_begin(TSDN_NULL,
			    &data->batcher, 1);
			assert_zu_ne(idx, BATCHER_NO_IDX,
			    "Batcher can't be full -- we have one of its "
			    "elems!");
			data->elems_data[idx] = (uint32_t)elem;
			batcher_push_end(TSDN_NULL, &data->batcher);
		}
	}

	/* Push all local elems back, flush local counts to the shared ones. */
	size_t push_idx = 0;
	if (nelems_owned != 0) {
		push_idx = batcher_push_begin(TSDN_NULL, &data->batcher,
		    nelems_owned);
		assert_zu_ne(BATCHER_NO_IDX, push_idx,
		    "Should be space to push");
	}
	for (size_t i = 0; i < STRESS_TEST_ELEMS; i++) {
		if (elems_owned[i]) {
			data->elems_data[push_idx] = (uint32_t)i;
			push_idx++;
			local_push_count[i]++;
			data->push_count[i]++;
		}
		atomic_fetch_add_zu(
		    &data->atomic_push_count[i], local_push_count[i],
		    ATOMIC_RELAXED);
		atomic_fetch_add_zu(
		    &data->atomic_pop_count[i], local_pop_count[i],
		    ATOMIC_RELAXED);
	}
	if (nelems_owned != 0) {
		batcher_push_end(TSDN_NULL, &data->batcher);
	}

	return NULL;
}

TEST_BEGIN(test_stress) {
	stress_test_data_t data;
	batcher_init(&data.batcher, STRESS_TEST_ELEMS);
	bool err = mtx_init(&data.pop_mtx);
	assert_false(err, "mtx_init failure");
	atomic_store_u32(&data.thread_id, 0, ATOMIC_RELAXED);
	for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
		data.push_count[i] = 0;
		data.pop_count[i] = 0;
		atomic_store_zu(&data.atomic_push_count[i], 0, ATOMIC_RELAXED);
		atomic_store_zu(&data.atomic_pop_count[i], 0, ATOMIC_RELAXED);

		size_t idx = batcher_push_begin(TSDN_NULL, &data.batcher, 1);
		assert_zu_eq(i, idx, "Should push in order");
		data.elems_data[idx] = i;
		batcher_push_end(TSDN_NULL, &data.batcher);
	}

	thd_t threads[STRESS_TEST_THREADS];
	for (int i = 0; i < STRESS_TEST_THREADS; i++) {
		thd_create(&threads[i], stress_test_thd, &data);
	}
	for (int i = 0; i < STRESS_TEST_THREADS; i++) {
		thd_join(threads[i], NULL);
	}
	for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
		assert_zu_ne(0, data.push_count[i],
		    "Should have done something!");
		assert_zu_eq(data.push_count[i], data.pop_count[i],
		    "every element should be pushed and popped an equal number "
		    "of times");
		assert_zu_eq(data.push_count[i],
		    atomic_load_zu(&data.atomic_push_count[i], ATOMIC_RELAXED),
		    "atomic and non-atomic count should be equal given proper "
		    "synchronization");
		assert_zu_eq(data.pop_count[i],
		    atomic_load_zu(&data.atomic_pop_count[i], ATOMIC_RELAXED),
		    "atomic and non-atomic count should be equal given proper "
		    "synchronization");
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_simple, test_multi_push, test_stress);
}
