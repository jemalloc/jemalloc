#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"
#ifdef JEMALLOC_PID_NAMESPACE
#include "jemalloc/internal/pid_namespace_externs.h"
#endif

#define TEST_PREFIX "test_prefix"

static bool did_prof_dump_open;

static int
prof_dump_open_file_intercept(const char *filename, int mode) {
	int fd;

	did_prof_dump_open = true;

	char filename_prefix[] = TEST_PREFIX ".";
#ifdef JEMALLOC_PID_NAMESPACE
    if (config_enable_pid_namespace) {
		ssize_t pid_ns = pid_namespace();
		int len = snprintf(NULL, 0, "%ld", pid_ns);
		snprintf(&filename_prefix[strlen(filename_prefix)], len + 1, "%ld", pid_ns);
	} else {
		int pid = getpid();
		int len = snprintf(NULL, 0, "%d", pid);
		snprintf(&filename_prefix[strlen(filename_prefix)], len + 1, "%d", pid);
	}
#else
	int pid = getpid();
	int len = snprintf(NULL, 0, "%d", pid);
	snprintf(&filename_prefix[strlen(filename_prefix)], len + 1, "%d", pid);
#endif
	expect_d_eq(strncmp(filename_prefix, filename, sizeof(filename_prefix)
	    - 1), 0, "Dump file name should start with \"" TEST_PREFIX ".\"");

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

TEST_BEGIN(test_pid_namespace_dump) {
	bool active;
	void *p;

	const char *test_prefix = TEST_PREFIX;

	test_skip_if(!config_prof);

	active = true;

	expect_d_eq(mallctl("prof.prefix", NULL, NULL, (void *)&test_prefix,
	    sizeof(test_prefix)), 0,
	    "Unexpected mallctl failure while overwriting dump prefix");

	expect_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0,
	    "Unexpected mallctl failure while activating profiling");

	prof_dump_open_file = prof_dump_open_file_intercept;

	did_prof_dump_open = false;
	p = mallocx(1, 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	dallocx(p, 0);
	expect_true(did_prof_dump_open, "Expected a profile dump");
}
TEST_END

int
main(void) {
	return test(
	    test_pid_namespace_dump);
}
