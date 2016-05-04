#include "test/jemalloc_test.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

TEST_BEGIN(test_fork)
{
#ifndef _WIN32
	void *p;
	pid_t pid;

	p = malloc(1);
	assert_ptr_not_null(p, "Unexpected malloc() failure");

	pid = fork();
	if (pid == -1) {
		/* Error. */
		test_fail("Unexpected fork() failure");
	} else if (pid == 0) {
		/* Child. */
		exit(0);
	} else {
		int status;

		/* Parent. */
		free(p);
		do {
			if (waitpid(pid, &status, 0) == -1)
				test_fail("Unexpected waitpid() failure");
		} while (!WIFEXITED(status) && !WIFSIGNALED(status));
	}
#else
	test_skip("fork(2) is irrelevant to Windows");
#endif
}
TEST_END

int
main(void)
{

	return (test(
	    test_fork));
}
