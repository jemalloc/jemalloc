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

int
main(void) {
	return test_no_reentrancy(test_fork, test_fork_child_usability,
	    test_fork_multithreaded, test_fork_postfork_descriptor_relink);
}
