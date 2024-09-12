#include "test/jemalloc_test.h"
#include "test/fork.h"

enum {
	STRESS_THREADS = 3,
	STRESS_OBJECTS_PER_THREAD = 1000,
	STRESS_ALLOC_SZ = PAGE / 2,
};

typedef struct stress_thread_data_s stress_thread_data_t;
struct stress_thread_data_s {
	unsigned thd_id;
	atomic_zu_t *ready_thds;
	atomic_zu_t *done_thds;
	void **to_dalloc;
};

static atomic_zu_t push_failure_count;
static atomic_zu_t pop_attempt_results[2];
static atomic_zu_t dalloc_zero_slab_count;
static atomic_zu_t dalloc_nonzero_slab_count;
static atomic_zu_t dalloc_nonempty_list_count;

static bool
should_skip() {
	return
	    /*
	     * We do batching operations on tcache flush pathways; we can't if
	     * caching is disabled.
	     */
	    !opt_tcache ||
	    /* We rely on tcache fill/flush operations of the size we use. */
	    opt_tcache_max < STRESS_ALLOC_SZ
	    /*
	     * Some of the races we want to trigger are fiddly enough that they
	     * only show up under real concurrency.  We add 1 to account for the
	     * main thread, which also does some work.
	     */
	    || ncpus < STRESS_THREADS + 1;
}

static void
increment_push_failure(size_t push_idx) {
	if (push_idx == BATCHER_NO_IDX) {
		atomic_fetch_add_zu(&push_failure_count, 1, ATOMIC_RELAXED);
	} else {
		assert_zu_lt(push_idx, 4, "Only 4 elems");
		volatile size_t x = 10000;
		while (--x) {
			/* Spin for a while, to try to provoke a failure. */
			if (x == push_idx) {
#ifdef _WIN32
				SwitchToThread();
#else
				sched_yield();
#endif
			}
		}
	}
}

static void
increment_pop_attempt(size_t elems_to_pop) {
	bool elems = (elems_to_pop != BATCHER_NO_IDX);
	atomic_fetch_add_zu(&pop_attempt_results[elems], 1, ATOMIC_RELAXED);
}

static void
increment_slab_dalloc_count(unsigned slab_dalloc_count, bool list_empty) {
	if (slab_dalloc_count > 0) {
		atomic_fetch_add_zu(&dalloc_nonzero_slab_count, 1,
		    ATOMIC_RELAXED);
	} else {
		atomic_fetch_add_zu(&dalloc_zero_slab_count, 1,
		    ATOMIC_RELAXED);
	}
	if (!list_empty) {
		atomic_fetch_add_zu(&dalloc_nonempty_list_count, 1,
		    ATOMIC_RELAXED);
	}
}

static void flush_tcache() {
	assert_d_eq(0, mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    "Unexpected mallctl failure");
}

static void *
stress_thread(void *arg) {
	stress_thread_data_t *data = arg;
	uint64_t prng_state = data->thd_id;
	atomic_fetch_add_zu(data->ready_thds, 1, ATOMIC_RELAXED);
	while (atomic_load_zu(data->ready_thds, ATOMIC_RELAXED)
	    != STRESS_THREADS) {
		/* Spin */
	}
	for (int i = 0; i < STRESS_OBJECTS_PER_THREAD; i++) {
		dallocx(data->to_dalloc[i], 0);
		if (prng_range_u64(&prng_state, 3) == 0) {
			flush_tcache();
		}

	}
	flush_tcache();
	atomic_fetch_add_zu(data->done_thds, 1, ATOMIC_RELAXED);
	return NULL;
}

/*
 * Run main_thread_fn in conditions that trigger all the various edge cases and
 * subtle race conditions.
 */
static void
stress_run(void (*main_thread_fn)(), int nruns) {
	bin_batching_test_ndalloc_slabs_max = 1;
	bin_batching_test_after_push_hook = &increment_push_failure;
	bin_batching_test_mid_pop_hook = &increment_pop_attempt;
	bin_batching_test_after_unlock_hook = &increment_slab_dalloc_count;

	atomic_store_zu(&push_failure_count, 0, ATOMIC_RELAXED);
	atomic_store_zu(&pop_attempt_results[0], 0, ATOMIC_RELAXED);
	atomic_store_zu(&pop_attempt_results[1], 0, ATOMIC_RELAXED);
	atomic_store_zu(&dalloc_zero_slab_count, 0, ATOMIC_RELAXED);
	atomic_store_zu(&dalloc_nonzero_slab_count, 0, ATOMIC_RELAXED);
	atomic_store_zu(&dalloc_nonempty_list_count, 0, ATOMIC_RELAXED);

	for (int run = 0; run < nruns; run++) {
		thd_t thds[STRESS_THREADS];
		stress_thread_data_t thd_datas[STRESS_THREADS];
		atomic_zu_t ready_thds;
		atomic_store_zu(&ready_thds, 0, ATOMIC_RELAXED);
		atomic_zu_t done_thds;
		atomic_store_zu(&done_thds, 0, ATOMIC_RELAXED);

		void *ptrs[STRESS_THREADS][STRESS_OBJECTS_PER_THREAD];
		for (int i = 0; i < STRESS_THREADS; i++) {
			thd_datas[i].thd_id = i;
			thd_datas[i].ready_thds = &ready_thds;
			thd_datas[i].done_thds = &done_thds;
			thd_datas[i].to_dalloc = ptrs[i];
			for (int j = 0; j < STRESS_OBJECTS_PER_THREAD; j++) {
				void *ptr = mallocx(STRESS_ALLOC_SZ, 0);
				assert_ptr_not_null(ptr, "alloc failure");
				ptrs[i][j] = ptr;
			}
		}
		for (int i = 0; i < STRESS_THREADS; i++) {
			thd_create(&thds[i], stress_thread, &thd_datas[i]);
		}
		while (atomic_load_zu(&done_thds, ATOMIC_RELAXED)
		    != STRESS_THREADS) {
			main_thread_fn();
		}
		for (int i = 0; i < STRESS_THREADS; i++) {
			thd_join(thds[i], NULL);
		}
	}

	bin_batching_test_ndalloc_slabs_max = (unsigned)-1;
	bin_batching_test_after_push_hook = NULL;
	bin_batching_test_mid_pop_hook = NULL;
	bin_batching_test_after_unlock_hook = NULL;
}

static void
do_allocs_frees() {
	enum {NALLOCS = 32};
	flush_tcache();
	void *ptrs[NALLOCS];
	for (int i = 0; i < NALLOCS; i++) {
		ptrs[i] = mallocx(STRESS_ALLOC_SZ, 0);
	}
	for (int i = 0; i < NALLOCS; i++) {
		dallocx(ptrs[i], 0);
	}
	flush_tcache();
}

static void
test_arena_reset_main_fn() {
	do_allocs_frees();
}

TEST_BEGIN(test_arena_reset) {
	int err;
	unsigned arena;
	unsigned old_arena;

	test_skip_if(should_skip());
	test_skip_if(opt_percpu_arena != percpu_arena_disabled);

	size_t arena_sz = sizeof(arena);
	err = mallctl("arenas.create", (void *)&arena, &arena_sz, NULL, 0);
	assert_d_eq(0, err, "Arena creation failed");

	err = mallctl("thread.arena", &old_arena, &arena_sz, &arena, arena_sz);
	assert_d_eq(0, err, "changing arena failed");

	stress_run(&test_arena_reset_main_fn, /* nruns */ 10);

	flush_tcache();

	char buf[100];
	malloc_snprintf(buf, sizeof(buf), "arena.%u.reset", arena);
	err = mallctl(buf, NULL, NULL, NULL, 0);
	assert_d_eq(0, err, "Couldn't change arena");

	do_allocs_frees();

	err = mallctl("thread.arena", NULL, NULL, &old_arena, arena_sz);
	assert_d_eq(0, err, "changing arena failed");
}
TEST_END

static void
test_fork_main_fn() {
#ifndef _WIN32
	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Fork failure!");
	} else if (pid == 0) {
		/* Child */
		do_allocs_frees();
		_exit(0);
	} else {
		fork_wait_for_child_exit(pid);
		do_allocs_frees();
	}
#endif
}

TEST_BEGIN(test_fork) {
#ifdef _WIN32
	test_skip("No fork on windows");
#endif
	test_skip_if(should_skip());
	stress_run(&test_fork_main_fn, /* nruns */ 10);
}
TEST_END

static void
test_races_main_fn() {
	do_allocs_frees();
}

TEST_BEGIN(test_races) {
	test_skip_if(should_skip());

	stress_run(&test_races_main_fn, /* nruns */ 400);

	assert_zu_lt(0, atomic_load_zu(&push_failure_count, ATOMIC_RELAXED),
	    "Should have seen some push failures");
	assert_zu_lt(0, atomic_load_zu(&pop_attempt_results[0], ATOMIC_RELAXED),
	    "Should have seen some pop failures");
	assert_zu_lt(0, atomic_load_zu(&pop_attempt_results[1], ATOMIC_RELAXED),
	    "Should have seen some pop successes");
	assert_zu_lt(0, atomic_load_zu(&dalloc_zero_slab_count, ATOMIC_RELAXED),
	    "Expected some frees that didn't empty a slab");
	assert_zu_lt(0, atomic_load_zu(&dalloc_nonzero_slab_count,
	    ATOMIC_RELAXED), "expected some frees that emptied a slab");
	assert_zu_lt(0, atomic_load_zu(&dalloc_nonempty_list_count,
	    ATOMIC_RELAXED), "expected some frees that used the empty list");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_arena_reset,
	    test_races,
	    test_fork);
}
