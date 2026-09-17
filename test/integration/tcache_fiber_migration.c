/*
 * Regression test for issue #2890: under whole-program LTO the inlined allocator
 * fastpath caches the TLS-derived tcache base in a register across a
 * swapcontext, so a fiber resumed on another OS thread uses the previous
 * thread's tcache -> heap corruption.  See JEMALLOC_TLS_ADDR in tsd_tls_addr.h.
 *
 * Standalone build:
 *
 *   clang -O2 -flto=thin -fuse-ld=lld -D_GNU_SOURCE -D_REENTRANT -DJEMALLOC_NO_PRIVATE_NAMESPACE \
 *         -Itest/include -Iinclude \
 *         $(find src -name '*.c' | grep -v zone.c) \
 *         test/integration/tcache_fiber_migration.c -pthread -lm \
 *         -o tcache_fiber_migration
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <ucontext.h>

#define JEMALLOC_MANGLE
#include <jemalloc/jemalloc.h>

enum {
	num_fibers = 128,
	num_workers = 8,
	ops_per_fiber = 20 * 1000,
	fiber_stack_size = 1 << 16,
	/* Power of two; a fiber sits in at most one queue at a time. */
	worker_queue_capacity = num_fibers
};

static ucontext_t fiber_context[num_fibers];
/* Scheduler context to switch back to when a fiber yields; the resuming worker
 * writes its own context here, so this changes when the fiber migrates. */
static ucontext_t *return_context[num_fibers];
static unsigned fiber_remaining_ops[num_fibers];
static bool fiber_done[num_fibers];

/*
 * Per-worker ready queues: a fiber that yields on worker w is enqueued to
 * worker (w + 1) % num_workers and can only be resumed there, so every resume
 * is a cross-thread migration by construction.
 */
static unsigned ready_queue[num_workers][worker_queue_capacity];
static unsigned ready_queue_head[num_workers];
static unsigned ready_queue_tail[num_workers];
static unsigned live_fibers;
static pthread_mutex_t queue_mtx = PTHREAD_MUTEX_INITIALIZER;

static void
push_ready_fiber(unsigned worker, unsigned id) {
	pthread_mutex_lock(&queue_mtx);
	ready_queue[worker][ready_queue_head[worker]++ &
	    (worker_queue_capacity - 1)] = id;
	pthread_mutex_unlock(&queue_mtx);
}

/* Returns a ready fiber id, -1 if none ready now, or -2 if all have finished. */
static int
pop_ready_fiber(unsigned worker) {
	int id;
	pthread_mutex_lock(&queue_mtx);
	if (live_fibers == 0) {
		id = -2;
	} else if (ready_queue_tail[worker] != ready_queue_head[worker]) {
		id = (int)ready_queue[worker][ready_queue_tail[worker]++ &
		    (worker_queue_capacity - 1)];
	} else {
		id = -1;
	}
	pthread_mutex_unlock(&queue_mtx);
	return id;
}

static void
fiber_run(int id) {
	void *p = malloc(64);
	while (fiber_remaining_ops[id] > 0) {
		fiber_remaining_ops[id]--;
		free(p); /* push to the CURRENT thread's tcache */
		/* Yield; resumed on the next worker thread (forced migration). */
		swapcontext(&fiber_context[id], return_context[id]);
		p = malloc(64); /* pop; must come from the NEW thread's tcache */
		*(char *)p = (char)id;
	}
	free(p);
	pthread_mutex_lock(&queue_mtx);
	fiber_done[id] = true;
	live_fibers--;
	pthread_mutex_unlock(&queue_mtx);
	swapcontext(&fiber_context[id], return_context[id]);
}

static void *
worker_thread(void *arg) {
	unsigned worker = (unsigned)(uintptr_t)arg;
	ucontext_t scheduler_context;
	for (;;) {
		int id = pop_ready_fiber(worker);
		if (id == -2) {
			break;
		}
		if (id < 0) {
			continue;
		}
		return_context[id] = &scheduler_context;
		swapcontext(&scheduler_context, &fiber_context[id]);
		if (!fiber_done[id]) {
			/* Hand off to the next worker: forced migration. */
			push_ready_fiber((worker + 1) % num_workers,
			    (unsigned)id);
		}
	}
	return NULL;
}

int
main(void) {
	void *stacks[num_fibers];
	pthread_t threads[num_workers];
	unsigned i;

	live_fibers = num_fibers;
	for (i = 0; i < num_fibers; i++) {
		stacks[i] = malloc(fiber_stack_size);
		fiber_remaining_ops[i] = ops_per_fiber;
		getcontext(&fiber_context[i]);
		fiber_context[i].uc_stack.ss_sp = stacks[i];
		fiber_context[i].uc_stack.ss_size = fiber_stack_size;
		fiber_context[i].uc_link = NULL;
		makecontext(&fiber_context[i], (void (*)(void))fiber_run, 1, (int)i);
		push_ready_fiber(i % num_workers, i);
	}

	for (i = 0; i < num_workers; i++) {
		pthread_create(&threads[i], NULL, worker_thread,
		    (void *)(uintptr_t)i);
	}
	for (i = 0; i < num_workers; i++) {
		pthread_join(threads[i], NULL);
	}
	for (i = 0; i < num_fibers; i++) {
		free(stacks[i]);
	}

	/* Completing without a tcache-corruption crash is success. */
	return live_fibers == 0 ? 0 : 2;
}
