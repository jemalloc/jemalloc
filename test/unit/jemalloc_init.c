#include "test/jemalloc_test.h"

#include "jemalloc/internal/jemalloc_init.h"

TEST_BEGIN(test_malloc_init_hard_idempotent) {
	expect_false(malloc_init_hard(),
	    "malloc_init_hard should return false when already initialized");
	expect_true(malloc_initialized(),
	    "malloc_initialized should still be true after re-calling "
	    "malloc_init_hard");
}
TEST_END

TEST_BEGIN(test_malloc_initializer_set_idempotent_on_main) {
	test_skip_if(!malloc_is_initializer());

	malloc_initializer_set();

	expect_true(malloc_is_initializer(),
	    "malloc_is_initializer should still be true after re-setting "
	    "from the same thread");
	expect_true(malloc_initializer_is_set(),
	    "malloc_initializer_is_set should still be true after re-setting");
}
TEST_END

#ifdef JEMALLOC_THREADED_INIT
static atomic_b_t initializer_worker_done;
static atomic_b_t initializer_worker_result;

static void *
is_initializer_worker(void *unused) {
	atomic_store_b(&initializer_worker_result, malloc_is_initializer(),
	    ATOMIC_RELEASE);
	atomic_store_b(&initializer_worker_done, true, ATOMIC_RELEASE);
	return NULL;
}
#endif

TEST_BEGIN(test_malloc_is_initializer_false_in_worker) {
#ifndef JEMALLOC_THREADED_INIT
	/*
	 * Without JEMALLOC_THREADED_INIT (e.g. macOS), malloc_initializer
	 * is a process-wide bool and malloc_is_initializer() returns true
	 * for every thread after init.  The per-thread distinction this
	 * test exercises only exists in threaded-init builds.
	 */
	test_skip_if(true);
#else
	test_skip_if(!malloc_is_initializer());

	atomic_store_b(&initializer_worker_done, false, ATOMIC_RELEASE);
	atomic_store_b(&initializer_worker_result, false, ATOMIC_RELEASE);

	thd_t thd;
	thd_create(&thd, is_initializer_worker, NULL);
	thd_join(thd, NULL);

	expect_true(
	    atomic_load_b(&initializer_worker_done, ATOMIC_ACQUIRE),
	    "Worker should have completed");
	expect_false(
	    atomic_load_b(&initializer_worker_result, ATOMIC_ACQUIRE),
	    "malloc_is_initializer should be false in a non-init thread");
#endif
}
TEST_END

int
main(void) {
	return test(test_malloc_init_hard_idempotent,
	    test_malloc_initializer_set_idempotent_on_main,
	    test_malloc_is_initializer_false_in_worker);
}
