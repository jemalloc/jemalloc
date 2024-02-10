#ifndef JEMALLOC_TEST_FORK_H
#define JEMALLOC_TEST_FORK_H

#ifndef _WIN32

#include <sys/wait.h>

static inline void
fork_wait_for_child_exit(int pid) {
	int status;
	while (true) {
		if (waitpid(pid, &status, 0) == -1) {
			test_fail("Unexpected waitpid() failure.");
		}
		if (WIFSIGNALED(status)) {
			test_fail("Unexpected child termination due to "
			    "signal %d", WTERMSIG(status));
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

#endif /* JEMALLOC_TEST_FORK_H */
