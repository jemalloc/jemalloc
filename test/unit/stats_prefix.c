#include "test/jemalloc_test.h"

#define TEST_PREFIX "stats_prefix_test"
#define TEST_OUT "stats_prefix_test.out"

#ifndef _WIN32
static unsigned nopen;
static char     last_filename[256];

/* Keep the first print in TEST_OUT; later ones go to /dev/null. */
static int
stats_open_file_intercept(const char *filename, int mode) {
	if (nopen++ == 0) {
		strncpy(last_filename, filename, sizeof(last_filename) - 1);
		return open(TEST_OUT, O_WRONLY | O_CREAT | O_TRUNC, mode);
	}
	return open("/dev/null", O_WRONLY);
}
#endif

TEST_BEGIN(test_stats_prefix) {
#ifdef _WIN32
	test_skip("opt.stats_prefix is not supported on Windows");
#else
	test_skip_if(!config_stats);
	test_skip_if(opt_stats_interval < 0);

	const char *prefix;
	size_t      sz = sizeof(prefix);
	expect_d_eq(mallctl("opt.stats_prefix", (void *)&prefix, &sz, NULL, 0),
	    0, "Unexpected mallctl() failure");
	expect_str_eq(prefix, TEST_PREFIX, "Unexpected opt.stats_prefix");

	nopen = 0;
	stats_open_file = stats_open_file_intercept;
	/* Allocate well past opt.stats_interval to trigger a print. */
	for (unsigned i = 0; i < 64 && nopen == 0; i++) {
		void *p = mallocx(64 << 10, 0);
		expect_ptr_not_null(p, "Unexpected mallocx() failure");
		dallocx(p, 0);
	}
	expect_u_gt(nopen, 0, "Expected interval-triggered stats output");

	char expected[64];
	malloc_snprintf(expected, sizeof(expected), TEST_PREFIX ".%d.",
	    (int)getpid());
	expect_d_eq(strncmp(last_filename, expected, strlen(expected)), 0,
	    "Unexpected file name \"%s\"", last_filename);
	const char *suffix = ".stats";
	size_t      len = strlen(last_filename);
	expect_zu_gt(len, strlen(suffix), "File name too short");
	expect_str_eq(last_filename + len - strlen(suffix), suffix,
	    "Unexpected file name \"%s\"", last_filename);

	char buf[4096];
	int  fd = open(TEST_OUT, O_RDONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	unlink(TEST_OUT);
	assert_zd_gt(n, 0, "Expected stats output in the file");
	buf[n] = '\0';
	expect_ptr_not_null(strstr(buf, "Begin jemalloc statistics"),
	    "Expected the stats header in the file");
#endif
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_stats_prefix);
}
