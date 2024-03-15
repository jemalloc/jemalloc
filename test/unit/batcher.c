#include "test/jemalloc_test.h"

#include "jemalloc/internal/batcher.h"

TEST_BEGIN(test_simple) {
    enum { NELEMS_MAX = 10, DATA_BASE_VAL = 100, NRUNS = 5 };
    batcher_t batcher;
    batcher_elem_t elems[NELEMS_MAX];
    int data[NELEMS_MAX];
    for (int nelems = 0; nelems < NELEMS_MAX; nelems++) {
        batcher_init(&batcher, elems, nelems);
        for (int run = 0; run < NRUNS; run++) {
            for (int i = 0; i < nelems; i++) {
                data[i] = -1;
            }
            for (int i = 0; i < nelems; i++) {
                int idx = batcher_push_begin(&batcher, elems);
                assert_d_eq(i, idx, "Wrong index");
                assert_d_eq(-1, data[idx], "Expected uninitialized slot");
                data[idx] = DATA_BASE_VAL + i;
                batcher_push_end(&batcher, elems, idx);
            }
            int idx = batcher_push_begin(&batcher, elems);
            assert_d_eq(BATCHER_NO_IDX, idx,
              "Shouldn't be able to push into a full batcher");

            batcher_pop_iter_t iter;
            bool nonempty = batcher_pop_begin(&batcher, elems, &iter);
            assert_b_eq(nelems != 0, nonempty, "Should have elements to pop");
            if (nonempty) {
                int nelems_seen = 0;
                while (true) {
                    int idx = batcher_pop_next(&batcher, elems, &iter);
                    if (idx == BATCHER_NO_IDX) {
                        break;
                    }
                    assert_d_eq(idx, nelems_seen, "Should pop in order pushed");
                    nelems_seen++;
                    assert_d_eq(
                      DATA_BASE_VAL + idx, data[idx], "Lost some data");
                }
                assert_d_eq(nelems, nelems_seen,
                  "Should pop same number of elements seen");
                batcher_pop_end(&batcher, elems, &iter);
            }
        }
    }
}
TEST_END

static void
assert_batcher_empty(batcher_t *batcher, batcher_elem_t *elems) {
    batcher_pop_iter_t iter;
    bool maybe_nonempty = batcher_pop_begin(batcher, elems, &iter);
    if (!maybe_nonempty) {
        return;
    }
    int idx = batcher_pop_next(batcher, elems, &iter);
    assert_d_eq(BATCHER_NO_IDX, idx, "Expected to find no elements");
    batcher_pop_end(batcher, elems, &iter);
}

TEST_BEGIN(test_pop_while_pushing) {
    enum { NELEMS = 2 };
    batcher_t batcher;
    batcher_elem_t elems[NELEMS];
    batcher_init(&batcher, elems, NELEMS);

    int idx;
    bool pop_elems;
    batcher_pop_iter_t iter;

    // Batcher is [E, E]

    pop_elems = batcher_pop_begin(&batcher, elems, &iter);
    assert_false(pop_elems, "Shouldn't pop before pushes");

    idx = batcher_push_begin(&batcher, elems);
    // Batcher is [P, E]
    assert_d_eq(0, idx, "Should push in order");
    idx = batcher_push_begin(&batcher, elems);
    // Batcher is [P, P]
    assert_d_eq(1, idx, "Should push in order");

    assert_batcher_empty(&batcher, elems);

    batcher_push_end(&batcher, elems, /* idx */ 1);

    // Batcher is [P, F]

    pop_elems = batcher_pop_begin(&batcher, elems, &iter);
    assert_true(pop_elems, "Expected an element ready");

    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(1, idx, "Element at idx 1 is only one ready");
    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(BATCHER_NO_IDX, idx, "Shouldn't have more elements left");
    batcher_pop_end(&batcher, elems, &iter);

    // Batcher is [P, E]

    assert_batcher_empty(&batcher, elems);

    idx = batcher_push_begin(&batcher, elems);
    // Batcher is [P, P]
    assert_d_eq(1, idx, "Only one free slot!");
    batcher_push_end(&batcher, elems, /* idx */ 1);

    // Batcher is [P, F]

    pop_elems = batcher_pop_begin(&batcher, elems, &iter);
    assert_true(pop_elems, "Expected an element ready");

    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(1, idx, "Element at idx 1 is only one ready");
    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(BATCHER_NO_IDX, idx, "Shouldn't have more elements left");
    batcher_pop_end(&batcher, elems, &iter);

    batcher_push_end(&batcher, elems, /* idx */ 0);
    pop_elems = batcher_pop_begin(&batcher, elems, &iter);
    assert_true(pop_elems, "Element at 0 should have finished push");

    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(0, idx, "Element at idx 0 is only one ready");
    idx = batcher_pop_next(&batcher, elems, &iter);
    assert_d_eq(BATCHER_NO_IDX, idx, "Shouldn't have more elements left");
    batcher_pop_end(&batcher, elems, &iter);
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
    batcher_elem_t elems[STRESS_TEST_ELEMS];
    mtx_t pop_mtx;
    atomic_u32_t thread_id;

    uint32_t elems_data[STRESS_TEST_ELEMS];
    size_t push_count[STRESS_TEST_ELEMS];
    size_t pop_count[STRESS_TEST_ELEMS];
    atomic_zu_t atomic_push_count[STRESS_TEST_ELEMS];
    atomic_zu_t atomic_pop_count[STRESS_TEST_ELEMS];
};

// Note: 0-indexed. If one element is set and you want to find it, you call
// get_nth_set(elems, 0).
static uint32_t
get_nth_set(bool elems_owned[STRESS_TEST_ELEMS], uint32_t n) {
    uint32_t ntrue = 0;
    for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
        if (elems_owned[i]) {
            ntrue++;
        }
        if (ntrue > n) {
            return i;
        }
    }
    assert_not_reached(
      "Asked for the %d'th set element when < %d are set", n, n);
    // Just to silence a compiler warning.
    return 0;
}

static void *
stress_test_thd(void *arg) {
    stress_test_data_t *data = arg;
    uint32_t prng = atomic_fetch_add_u32(&data->thread_id, 1, ATOMIC_RELAXED);

    uint32_t nelems_owned = 0;
    bool elems_owned[STRESS_TEST_ELEMS] = {0};
    size_t local_push_count[STRESS_TEST_ELEMS] = {0};
    size_t local_pop_count[STRESS_TEST_ELEMS] = {0};

    for (int i = 0; i < STRESS_TEST_OPS; i++) {
        uint32_t rnd = prng_range_u32(&prng, STRESS_TEST_PUSH_TO_POP_RATIO);
        if (rnd == 0 || nelems_owned == 0) {
            // Pop all.
            mtx_lock(&data->pop_mtx);
            batcher_pop_iter_t iter;
            bool elems = batcher_pop_begin(&data->batcher, data->elems, &iter);
            if (elems) {
                while (true) {
                    int idx =
                      batcher_pop_next(&data->batcher, data->elems, &iter);
                    if (idx == BATCHER_NO_IDX) {
                        break;
                    }
                    uint32_t elem = data->elems_data[idx];
                    assert_false(elems_owned[elem],
                      "Shouldn't already own what we just popped");
                    elems_owned[elem] = true;
                    nelems_owned++;
                    local_pop_count[elem]++;
                    data->pop_count[elem]++;
                }
                batcher_pop_end(&data->batcher, data->elems, &iter);
            }
            mtx_unlock(&data->pop_mtx);
        } else {
            // Push
            uint32_t elem_to_push_idx = prng_range_u32(&prng, nelems_owned);
            uint32_t elem = get_nth_set(elems_owned, elem_to_push_idx);
            assert_true(
              elems_owned[elem], "Should own element we're about to pop");
            elems_owned[elem] = false;
            local_push_count[elem]++;
            data->push_count[elem]++;
            nelems_owned--;
            int idx = batcher_push_begin(&data->batcher, data->elems);
            assert_d_ne(idx, BATCHER_NO_IDX,
              "Batcher can't be full -- we have one of its elems!");
            data->elems_data[idx] = elem;
            batcher_push_end(&data->batcher, data->elems, idx);
        }
    }

    // Push all local elems back, flush local counts to the shared ones.
    for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
        if (elems_owned[i]) {
            int idx = batcher_push_begin(&data->batcher, data->elems);
            assert_d_ne(BATCHER_NO_IDX, idx, "Should be space to push");
            data->elems_data[idx] = i;
            local_push_count[i]++;
            data->push_count[i]++;
            batcher_push_end(&data->batcher, data->elems, idx);
        }
        atomic_fetch_add_zu(
          &data->atomic_push_count[i], local_push_count[i], ATOMIC_RELAXED);
        atomic_fetch_add_zu(
          &data->atomic_pop_count[i], local_pop_count[i], ATOMIC_RELAXED);
    }

    return NULL;
}

TEST_BEGIN(test_stress) {
    stress_test_data_t data;
    batcher_init(&data.batcher, data.elems, STRESS_TEST_ELEMS);
    bool err = mtx_init(&data.pop_mtx);
    assert_false(err, "mtx_init failure");
    atomic_store_u32(&data.thread_id, 0, ATOMIC_RELAXED);
    for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
        data.push_count[i] = 0;
        data.pop_count[i] = 0;
        atomic_store_zu(&data.atomic_push_count[i], 0, ATOMIC_RELAXED);
        atomic_store_zu(&data.atomic_pop_count[i], 0, ATOMIC_RELAXED);

        int idx = batcher_push_begin(&data.batcher, data.elems);
        assert_d_eq(i, idx, "Should push in order");
        data.elems_data[idx] = i;
        batcher_push_end(&data.batcher, data.elems, idx);
    }

    thd_t threads[STRESS_TEST_THREADS];
    for (int i = 0; i < STRESS_TEST_THREADS; i++) {
        thd_create(&threads[i], stress_test_thd, &data);
    }
    for (int i = 0; i < STRESS_TEST_THREADS; i++) {
        thd_join(threads[i], NULL);
    }
    for (int i = 0; i < STRESS_TEST_ELEMS; i++) {
        assert_zu_ne(0, data.push_count[i], "Should have done something!");
        assert_zu_eq(data.push_count[i], data.pop_count[i],
          "every element should be pushed and popped an equal number of times");
        assert_zu_eq(data.push_count[i],
          atomic_load_zu(&data.atomic_push_count[i], ATOMIC_RELAXED),
          "atomic and non-atomic count should be equal given proper synchronization");
        assert_zu_eq(data.pop_count[i],
          atomic_load_zu(&data.atomic_pop_count[i], ATOMIC_RELAXED),
          "atomic and non-atomic count should be equal given proper synchronization");
    }
}
TEST_END

int
main(void) {
    return test_no_reentrancy(test_simple, test_pop_while_pushing, test_stress);
}
