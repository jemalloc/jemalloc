#include "test/jemalloc_test.h"
#include "test/bench.h"

#define BATCH (1000 * 1000)
#define HUGE_BATCH (100 * BATCH)
static void *batch_ptrs[HUGE_BATCH];
static void *item_ptrs[HUGE_BATCH];

#define SIZE 7

typedef struct batch_alloc_packet_s batch_alloc_packet_t;
struct batch_alloc_packet_s {
	void **ptrs;
	size_t num;
	size_t size;
	int flags;
};

static void
batch_alloc_wrapper(size_t batch) {
	batch_alloc_packet_t batch_alloc_packet = {batch_ptrs, batch, SIZE, 0};
	size_t filled;
	size_t len = sizeof(size_t);
	assert_d_eq(mallctl("experimental.batch_alloc", &filled, &len,
	    &batch_alloc_packet, sizeof(batch_alloc_packet)), 0, "");
	assert_zu_eq(filled, batch, "");
}

static void
item_alloc_wrapper(size_t batch) {
	for (size_t i = 0; i < batch; ++i) {
		item_ptrs[i] = malloc(SIZE);
	}
}

static void
release_and_clear(void **ptrs, size_t batch) {
	for (size_t i = 0; i < batch; ++i) {
		void *p = ptrs[i];
		assert_ptr_not_null(p, "allocation failed");
		sdallocx(p, SIZE, 0);
		ptrs[i] = NULL;
	}
}

static void
batch_alloc_small_can_repeat() {
	batch_alloc_wrapper(BATCH);
	release_and_clear(batch_ptrs, BATCH);
}

static void
item_alloc_small_can_repeat() {
	item_alloc_wrapper(BATCH);
	release_and_clear(item_ptrs, BATCH);
}

TEST_BEGIN(test_small_batch_with_free) {
	compare_funcs(10, 100,
	    "batch allocation", batch_alloc_small_can_repeat,
	    "item allocation", item_alloc_small_can_repeat);
}
TEST_END

static void
batch_alloc_huge_cannot_repeat() {
	batch_alloc_wrapper(HUGE_BATCH);
}

static void
item_alloc_huge_cannot_repeat() {
	item_alloc_wrapper(HUGE_BATCH);
}

TEST_BEGIN(test_huge_batch_without_free) {
	compare_funcs(0, 1,
	    "batch allocation", batch_alloc_huge_cannot_repeat,
	    "item allocation", item_alloc_huge_cannot_repeat);
	release_and_clear(batch_ptrs, HUGE_BATCH);
	release_and_clear(item_ptrs, HUGE_BATCH);
}
TEST_END

int main(void) {
	return test_no_reentrancy(
	    test_small_batch_with_free,
	    test_huge_batch_without_free);
}
