#include "test/jemalloc_test.h"

#define TEST_PREFIX "test_prefix"

static bool did_prof_dump_open;

static int
prof_dump_open_intercept(bool propagate_err, const char *filename) {
	int fd;

	did_prof_dump_open = true;

	const char filename_prefix[] = TEST_PREFIX ".";
	assert_d_eq(strncmp(filename_prefix, filename, sizeof(filename_prefix)
	    - 1), 0, "Dump file name should start with \"" TEST_PREFIX ".\"");

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

TEST_BEGIN(test_idump) {
	bool active;
	void *p;

	const char *dump_prefix = TEST_PREFIX;

	test_skip_if(!config_prof);

	active = true;

	assert_d_eq(mallctl("prof.dump_prefix", NULL, NULL,
	    (void *)&dump_prefix, sizeof(dump_prefix)), 0,
	    "Unexpected mallctl failure while overwriting dump prefix");

	assert_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0,
	    "Unexpected mallctl failure while activating profiling");

	prof_dump_open = prof_dump_open_intercept;

	did_prof_dump_open = false;
	p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	dallocx(p, 0);
	assert_true(did_prof_dump_open, "Expected a profile dump");
}
TEST_END

int
main(void) {
	return test(
	    test_idump);
}
