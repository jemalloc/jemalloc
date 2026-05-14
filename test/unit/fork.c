#include "test/jemalloc_test.h"

#ifndef _WIN32
#	include <sys/wait.h>
#endif

#ifndef _WIN32
static void
wait_for_child_exit(int pid) {
	int status;
	while (true) {
		if (waitpid(pid, &status, 0) == -1) {
			test_fail("Unexpected waitpid() failure.");
		}
		if (WIFSIGNALED(status)) {
			test_fail(
			    "Unexpected child termination due to "
			    "signal %d",
			    WTERMSIG(status));
			break;
		}
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0) {
				test_fail("Unexpected child exit value %d",
				    WEXITSTATUS(status));
			}
			break;
		}
	}
}
#endif

#ifndef _WIN32
static void
create_arena(unsigned *arena_ind) {
	size_t sz = sizeof(*arena_ind);
	expect_d_eq(mallctl("arenas.create", (void *)arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
}

static void
bind_thread_arena(unsigned arena_ind) {
	unsigned old_arena_ind;
	size_t   sz = sizeof(old_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
	                (void *)&arena_ind, sizeof(arena_ind)),
	    0, "Unexpected mallctl() failure");
}

static void
populate_tcache(void) {
	void *p[16];
	for (size_t i = 0; i < ARRAY_SIZE(p); i++) {
		p[i] = malloc(8);
		expect_ptr_not_null(p[i], "Unexpected malloc() failure");
	}
	for (size_t i = 0; i < ARRAY_SIZE(p); i++) {
		free(p[i]);
	}
}

static bool
refresh_epoch(void) {
	uint64_t epoch = 1;
	return mallctl("epoch", NULL, NULL, &epoch, sizeof(epoch)) == 0;
}

static int
read_arena_tcache_bytes(unsigned arena_ind, size_t *bytes) {
	char ctl[64];
	malloc_snprintf(ctl, sizeof(ctl),
	    "stats.arenas.%u.tcache_bytes", arena_ind);
	size_t sz = sizeof(*bytes);
	return mallctl(ctl, bytes, &sz, NULL, 0);
}

static int
read_arena_nthreads(unsigned arena_ind, unsigned *nthreads) {
	char ctl[64];
	malloc_snprintf(ctl, sizeof(ctl), "stats.arenas.%u.nthreads",
	    arena_ind);
	size_t sz = sizeof(*nthreads);
	return mallctl(ctl, nthreads, &sz, NULL, 0);
}
#endif

TEST_BEGIN(test_fork) {
#ifndef _WIN32
	void *p;
	pid_t pid;

	/* Set up a manually managed arena for test. */
	unsigned arena_ind;
	size_t   sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	/* Migrate to the new arena. */
	unsigned old_arena_ind;
	sz = sizeof(old_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
	                (void *)&arena_ind, sizeof(arena_ind)),
	    0, "Unexpected mallctl() failure");

	p = malloc(1);
	expect_ptr_not_null(p, "Unexpected malloc() failure");

	pid = fork();

	free(p);

	p = malloc(64);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	free(p);

	if (pid == -1) {
		/* Error. */
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child. */
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

#ifndef _WIN32
static void *
do_fork_thd(void *arg) {
	malloc(1);
	int pid = fork();
	if (pid == -1) {
		/* Error. */
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child. */
		char *args[] = {"true", NULL};
		execvp(args[0], args);
		test_fail("Exec failed");
	} else {
		/* Parent */
		wait_for_child_exit(pid);
	}
	return NULL;
}
#endif

#ifndef _WIN32
static void
do_test_fork_multithreaded(void) {
	thd_t child;
	thd_create(&child, do_fork_thd, NULL);
	do_fork_thd(NULL);
	thd_join(child, NULL);
}
#endif

TEST_BEGIN(test_fork_child_usability) {
#ifndef _WIN32
	void    *p;
	pid_t    pid;
	unsigned arena_ind;
	size_t   sz = sizeof(unsigned);

	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	p = mallocx(1, MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");

	pid = fork();

	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child: exercise non-trivial allocator operations. */

		/* Free pre-fork pointer (arena mutexes must be unlocked). */
		dallocx(p, MALLOCX_TCACHE_NONE);

		/* Basic malloc/free. */
		void *q = malloc(64);
		if (q == NULL) {
			_exit(1);
		}
		free(q);

		/* Create a new arena (ctl_mtx + arenas_lock re-init). */
		unsigned child_arena;
		size_t   csz = sizeof(unsigned);
		if (mallctl("arenas.create", (void *)&child_arena, &csz,
		        NULL, 0) != 0) {
			_exit(2);
		}

		/* Allocate from the new arena. */
		q = mallocx(128,
		    MALLOCX_ARENA(child_arena) | MALLOCX_TCACHE_NONE);
		if (q == NULL) {
			_exit(3);
		}
		dallocx(q, MALLOCX_TCACHE_NONE);

		/* Read narenas (ctl read path). */
		unsigned narenas;
		csz = sizeof(unsigned);
		if (mallctl("arenas.narenas", (void *)&narenas, &csz,
		        NULL, 0) != 0) {
			_exit(4);
		}

		/* Destroy the child-created arena. */
		size_t mib[3];
		size_t miblen = ARRAY_SIZE(mib);
		if (mallctlnametomib("arena.0.destroy", mib, &miblen)
		    != 0) {
			_exit(5);
		}
		mib[1] = (size_t)child_arena;
		if (mallctlbymib(mib, miblen, NULL, NULL, NULL, 0) != 0) {
			_exit(6);
		}

		_exit(0);
	} else {
		/* Parent. */
		dallocx(p, MALLOCX_TCACHE_NONE);
		wait_for_child_exit(pid);
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

TEST_BEGIN(test_fork_multithreaded) {
#ifndef _WIN32
	/*
	 * We've seen bugs involving hanging on arenas_lock (though the same
	 * class of bugs can happen on any mutex).  The bugs are intermittent
	 * though, so we want to run the test multiple times.  Since we hold the
	 * arenas lock only early in the process lifetime, we can't just run
	 * this test in a loop (since, after all the arenas are initialized, we
	 * won't acquire arenas_lock any further).  We therefore repeat the test
	 * with multiple processes.
	 */
	for (int i = 0; i < 100; i++) {
		int pid = fork();
		if (pid == -1) {
			/* Error. */
			test_fail("Unexpected fork() failure,");
		} else if (pid == 0) {
			/* Child. */
			do_test_fork_multithreaded();
			_exit(0);
		} else {
			wait_for_child_exit(pid);
		}
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

TEST_BEGIN(test_fork_postfork_descriptor_relink) {
#ifndef _WIN32
	test_skip_if(!config_stats);
	test_skip_if(!tcache_available(tsd_fetch()));

	/* Set up a manually managed arena bound to this thread. */
	unsigned arena_ind;
	size_t   sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");

	unsigned old_arena_ind;
	sz = sizeof(old_arena_ind);
	expect_d_eq(mallctl("thread.arena", (void *)&old_arena_ind, &sz,
	                (void *)&arena_ind, sizeof(arena_ind)),
	    0, "Unexpected mallctl() failure");

	/* Populate the cache_bin so the descriptor has cached bytes. */
	void *p[16];
	for (size_t i = 0; i < ARRAY_SIZE(p); i++) {
		p[i] = malloc(8);
		expect_ptr_not_null(p[i], "Unexpected malloc() failure");
	}
	for (size_t i = 0; i < ARRAY_SIZE(p); i++) {
		free(p[i]);
	}

	/* Sanity: parent's stats walk finds cached bytes via the queue. */
	uint64_t epoch = 1;
	expect_d_eq(mallctl("epoch", NULL, NULL, &epoch, sizeof(uint64_t)),
	    0, "epoch refresh failed");
	char ctl[64];
	malloc_snprintf(ctl, sizeof(ctl),
	    "stats.arenas.%u.tcache_bytes", arena_ind);
	size_t parent_bytes = 0;
	size_t bsz = sizeof(parent_bytes);
	expect_d_eq(mallctl(ctl, &parent_bytes, &bsz, NULL, 0),
	    0, "read tcache_bytes pre-fork");
	expect_zu_gt(parent_bytes, 0,
	    "Parent should see cached bytes from cache_bin descriptor");

	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/*
		 * Child: verify the surviving descriptor is still in the
		 * arena's queue. If postfork relink failed, the stats walk
		 * would find an empty queue and report 0 cached bytes.
		 */
		void *q = malloc(8);
		if (q == NULL) {
			_exit(1);
		}
		free(q);

		uint64_t child_epoch = 1;
		if (mallctl("epoch", NULL, NULL, &child_epoch,
		        sizeof(uint64_t)) != 0) {
			_exit(2);
		}
		size_t child_bytes = 0;
		size_t cbsz = sizeof(child_bytes);
		if (mallctl(ctl, &child_bytes, &cbsz, NULL, 0) != 0) {
			_exit(3);
		}
		if (child_bytes == 0) {
			_exit(4);
		}
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

#ifndef _WIN32
typedef struct {
	unsigned   arena_ind;
	atomic_b_t ready;
	atomic_b_t release;
} fork_worker_arg_t;

static void *
fork_worker_thd(void *arg) {
	fork_worker_arg_t *worker = (fork_worker_arg_t *)arg;

	bind_thread_arena(worker->arena_ind);
	populate_tcache();
	atomic_store_b(&worker->ready, true, ATOMIC_RELEASE);

	while (!atomic_load_b(&worker->release, ATOMIC_ACQUIRE)) {
		sleep_ns(1000);
	}
	return NULL;
}
#endif

TEST_BEGIN(test_fork_postfork_descriptor_relink_multithreaded) {
#ifndef _WIN32
	test_skip_if(!config_stats);
	test_skip_if(!tcache_available(tsd_fetch()));

	unsigned survivor_arena;
	unsigned peer_arena;
	create_arena(&survivor_arena);
	create_arena(&peer_arena);

	bind_thread_arena(survivor_arena);
	populate_tcache();

	fork_worker_arg_t survivor_worker = {
		.arena_ind = survivor_arena,
		.ready = ATOMIC_INIT(false),
		.release = ATOMIC_INIT(false),
	};
	fork_worker_arg_t peer_worker = {
		.arena_ind = peer_arena,
		.ready = ATOMIC_INIT(false),
		.release = ATOMIC_INIT(false),
	};
	thd_t survivor_thd;
	thd_t peer_thd;
	thd_create(&survivor_thd, fork_worker_thd, &survivor_worker);
	thd_create(&peer_thd, fork_worker_thd, &peer_worker);

	while (!atomic_load_b(&survivor_worker.ready, ATOMIC_ACQUIRE)
	    || !atomic_load_b(&peer_worker.ready, ATOMIC_ACQUIRE)) {
		sleep_ns(1000);
	}

	expect_true(refresh_epoch(), "epoch refresh failed");

	size_t pre_survivor_bytes = 0;
	size_t pre_peer_bytes = 0;
	unsigned pre_survivor_nthreads = 0;
	unsigned pre_peer_nthreads = 0;
	expect_d_eq(read_arena_tcache_bytes(
	                survivor_arena, &pre_survivor_bytes),
	    0, "read survivor tcache_bytes pre-fork");
	expect_d_eq(read_arena_tcache_bytes(peer_arena, &pre_peer_bytes), 0,
	    "read peer tcache_bytes pre-fork");
	expect_d_eq(read_arena_nthreads(
	                survivor_arena, &pre_survivor_nthreads),
	    0, "read survivor nthreads pre-fork");
	expect_d_eq(read_arena_nthreads(peer_arena, &pre_peer_nthreads), 0,
	    "read peer nthreads pre-fork");
	expect_zu_gt(pre_survivor_bytes, 0,
	    "Survivor arena should have cached bytes before fork");
	expect_zu_gt(pre_peer_bytes, 0,
	    "Peer arena should have cached bytes before fork");
	expect_u_eq(pre_survivor_nthreads, 2,
	    "Survivor arena should have two threads before fork");
	expect_u_eq(pre_peer_nthreads, 1,
	    "Peer arena should have one thread before fork");

	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		if (!refresh_epoch()) {
			_exit(1);
		}

		size_t child_survivor_bytes = 0;
		size_t child_peer_bytes = 0;
		unsigned child_survivor_nthreads = 0;
		unsigned child_peer_nthreads = 0;
		if (read_arena_tcache_bytes(
		        survivor_arena, &child_survivor_bytes) != 0) {
			_exit(2);
		}
		if (read_arena_tcache_bytes(peer_arena, &child_peer_bytes)
		    != 0) {
			_exit(3);
		}
		if (read_arena_nthreads(
		        survivor_arena, &child_survivor_nthreads) != 0) {
			_exit(4);
		}
		if (read_arena_nthreads(peer_arena, &child_peer_nthreads)
		    != 0) {
			_exit(5);
		}
		if (child_survivor_nthreads != 1) {
			_exit(6);
		}
		if (child_peer_nthreads != 0) {
			_exit(7);
		}
		if (child_survivor_bytes == 0) {
			_exit(8);
		}
		/*
		 * Don't compare child_survivor_bytes against pre_survivor_bytes:
		 * on platforms where jemalloc is the default zone allocator
		 * (macOS), internal allocations during the postfork handler
		 * (e.g. pthread_mutex_init) can inflate the surviving thread's
		 * tcache, making the child's bytes >= the pre-fork total.
		 */
		if (child_peer_bytes != 0) {
			_exit(9);
		}
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}

	expect_true(refresh_epoch(), "epoch refresh failed");

	size_t parent_survivor_bytes = 0;
	size_t parent_peer_bytes = 0;
	unsigned parent_survivor_nthreads = 0;
	unsigned parent_peer_nthreads = 0;
	expect_d_eq(read_arena_tcache_bytes(
	                survivor_arena, &parent_survivor_bytes),
	    0, "read survivor tcache_bytes post-fork");
	expect_d_eq(read_arena_tcache_bytes(peer_arena, &parent_peer_bytes), 0,
	    "read peer tcache_bytes post-fork");
	expect_d_eq(read_arena_nthreads(
	                survivor_arena, &parent_survivor_nthreads),
	    0, "read survivor nthreads post-fork");
	expect_d_eq(read_arena_nthreads(peer_arena, &parent_peer_nthreads), 0,
	    "read peer nthreads post-fork");
	expect_zu_eq(parent_survivor_bytes, pre_survivor_bytes,
	    "Parent survivor arena cached bytes should be unchanged");
	expect_zu_eq(parent_peer_bytes, pre_peer_bytes,
	    "Parent peer arena cached bytes should be unchanged");
	expect_u_eq(parent_survivor_nthreads, 2,
	    "Parent survivor arena should still have two threads");
	expect_u_eq(parent_peer_nthreads, 1,
	    "Parent peer arena should still have one thread");

	atomic_store_b(&survivor_worker.release, true, ATOMIC_RELEASE);
	atomic_store_b(&peer_worker.release, true, ATOMIC_RELEASE);
	thd_join(survivor_thd, NULL);
	thd_join(peer_thd, NULL);
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

TEST_BEGIN(test_fork_postfork_double_fork) {
#ifndef _WIN32
	test_skip_if(!config_stats);

	unsigned arena_ind;
	create_arena(&arena_ind);
	bind_thread_arena(arena_ind);
	populate_tcache();

	pid_t pid = fork();
	if (pid == -1) {
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* First child: verify nthreads, then fork again. */
		if (!refresh_epoch()) {
			_exit(1);
		}
		unsigned nthreads = 0;
		if (read_arena_nthreads(arena_ind, &nthreads) != 0) {
			_exit(2);
		}
		if (nthreads != 1) {
			_exit(3);
		}

		pid_t pid2 = fork();
		if (pid2 == -1) {
			_exit(4);
		} else if (pid2 == 0) {
			/* Grandchild: verify nthreads again. */
			if (!refresh_epoch()) {
				_exit(1);
			}
			unsigned gc_nthreads = 0;
			if (read_arena_nthreads(arena_ind, &gc_nthreads)
			    != 0) {
				_exit(2);
			}
			if (gc_nthreads != 1) {
				_exit(3);
			}
			_exit(0);
		} else {
			wait_for_child_exit(pid2);
		}
		_exit(0);
	} else {
		wait_for_child_exit(pid);
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_fork, test_fork_child_usability,
	    test_fork_multithreaded, test_fork_postfork_descriptor_relink,
	    test_fork_postfork_descriptor_relink_multithreaded,
	    test_fork_postfork_double_fork);
}
