#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

static const char *test_filename = "test_filename";
static bool        did_prof_dump_open;

TEST_BEGIN(test_prof_parse_pid_namespace) {
	struct test_case_s {
		const char *input;
		size_t      buf_size;
		ssize_t     linklen;
		uint64_t    expected;
	} test_cases[] = {
	    /* A typical valid namespace ID. */
	    {"pid:[1234]", sizeof("pid:[1234]"),
	                     sizeof("pid:[1234]") - 1, UINT64_C(1234)},
	    /* The shortest valid namespace ID contains one digit. */
	    {"pid:[1]", sizeof("pid:[1]"), sizeof("pid:[1]") - 1,
	        UINT64_C(1)},
	    /* Leading zeroes do not change the parsed value. */
	    {"pid:[0001234]", sizeof("pid:[0001234]"),
	        sizeof("pid:[0001234]") - 1, UINT64_C(1234)},
	    /* A realistic namespace ID must fit on 32-bit platforms. */
	    {"pid:[4026531836]", sizeof("pid:[4026531836]"),
	        sizeof("pid:[4026531836]") - 1, UINT64_C(4026531836)},
	    /* UINT64_MAX is the largest representable namespace ID. */
	    {"pid:[18446744073709551615]",
	        sizeof("pid:[18446744073709551615]"),
	        sizeof("pid:[18446744073709551615]") - 1, UINT64_MAX},
	    /* Reject an incorrect prefix. */
	    {"pid-:[1234]", sizeof("pid-:[1234]"),
	        sizeof("pid-:[1234]") - 1, 0},
	    /* Reject non-digit characters within the namespace ID. */
	    {"pid:[12a4]", sizeof("pid:[12a4]"), sizeof("pid:[12a4]") - 1,
	        0},
	    /* Reject an explicit positive sign, only digits are allowed. */
	    {"pid:[+1234]", sizeof("pid:[+1234]"),
	        sizeof("pid:[+1234]") - 1, 0},
	    /* Reject negative namespace IDs. */
	    {"pid:[-1]", sizeof("pid:[-1]"), sizeof("pid:[-1]") - 1, 0},
	    /* Reject a namespace ID with no digits. */
	    {"pid:[]", sizeof("pid:[]"), sizeof("pid:[]") - 1, 0},
	    /* Reject an input without the closing bracket. */
	    {"pid:[1234", sizeof("pid:[1234"), sizeof("pid:[1234") - 1, 0},
	    /* Reject trailing content included within linklen. */
	    {"pid:[1234]x", sizeof("pid:[1234]x"),
	        sizeof("pid:[1234]x") - 1, 0},
	    /* Reject a value one greater than UINT64_MAX. */
	    {"pid:[18446744073709551616]",
	        sizeof("pid:[18446744073709551616]"),
	        sizeof("pid:[18446744073709551616]") - 1, 0},
	    /* Only bytes before linklen belong to the symlink target. */
	    {"pid:[12]junk", sizeof("pid:[12]junk"), sizeof("pid:[12]") - 1,
	        UINT64_C(12)},
	    /* Reject linklen equal to buf_size as potentially truncated. */
	    {"pid:[1234]", sizeof("pid:[1234]") - 1,
	        sizeof("pid:[1234]") - 1, 0},
	    /* A zero linklen means readlink returned no data. */
	    {"pid:[1234]", sizeof("pid:[1234]"), 0, 0},
	    /* A negative linklen represents a readlink error. */
	    {"pid:[1234]", sizeof("pid:[1234]"), -1, 0}};

	size_t test_case_count = ARRAY_SIZE(test_cases);
	for (size_t i = 0; i < test_case_count; i++) {
		struct test_case_s *test_case = &test_cases[i];
		expect_u64_eq(os_prof_parse_pid_namespace(test_case->input,
		                 test_case->buf_size, test_case->linklen),
		    test_case->expected, "Unexpected result for test case %zu",
		    i);
	}
}
TEST_END

static int
prof_dump_open_file_intercept(const char *filename, int mode) {
	int fd;

	did_prof_dump_open = true;

	/*
	 * Stronger than a strcmp() - verifying that we internally directly use
	 * the caller supplied char pointer.
	 */
	expect_ptr_eq(filename, test_filename,
	    "Dump file name should be \"%s\"", test_filename);

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

TEST_BEGIN(test_mdump_normal) {
	test_skip_if(!config_prof);

	prof_dump_open_file_t *open_file_orig = prof_dump_open_file;

	void *p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	prof_dump_open_file = prof_dump_open_file_intercept;
	did_prof_dump_open = false;
	expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&test_filename,
	                sizeof(test_filename)),
	    0, "Unexpected mallctl failure while dumping");
	expect_true(did_prof_dump_open, "Expected a profile dump");

	dallocx(p, 0);

	prof_dump_open_file = open_file_orig;
}
TEST_END

static int
prof_dump_open_file_error(const char *filename, int mode) {
	return -1;
}

/*
 * In the context of test_mdump_output_error, prof_dump_write_file_count is the
 * total number of times prof_dump_write_file_error() is expected to be called.
 * In the context of test_mdump_maps_error, prof_dump_write_file_count is the
 * total number of times prof_dump_write_file_error() is expected to be called
 * starting from the one that contains an 'M' (beginning the "MAPPED_LIBRARIES"
 * header).
 */
static int prof_dump_write_file_count;

static ssize_t
prof_dump_write_file_error(int fd, const void *s, size_t len) {
	--prof_dump_write_file_count;

	expect_d_ge(prof_dump_write_file_count, 0,
	    "Write is called after error occurs");

	if (prof_dump_write_file_count == 0) {
		return -1;
	} else {
		/*
		 * Any non-negative number indicates success, and for
		 * simplicity we just use 0.  When prof_dump_write_file_count
		 * is positive, it means that we haven't reached the write that
		 * we want to fail; when prof_dump_write_file_count is
		 * negative, it means that we've already violated the
		 * expect_d_ge(prof_dump_write_file_count, 0) statement above,
		 * but instead of aborting, we continue the rest of the test,
		 * and we indicate that all the writes after the failed write
		 * are successful.
		 */
		return 0;
	}
}

static void
expect_write_failure(int count) {
	prof_dump_write_file_count = count;
	expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&test_filename,
	                sizeof(test_filename)),
	    EFAULT, "Dump should err");
	expect_d_eq(prof_dump_write_file_count, 0,
	    "Dumping stopped after a wrong number of writes");
}

TEST_BEGIN(test_mdump_output_error) {
	test_skip_if(!config_prof);
	test_skip_if(!config_debug);

	prof_dump_open_file_t  *open_file_orig = prof_dump_open_file;
	prof_dump_write_file_t *write_file_orig = prof_dump_write_file;

	prof_dump_write_file = prof_dump_write_file_error;

	void *p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	/*
	 * When opening the dump file fails, there shouldn't be any write, and
	 * mallctl() should return failure.
	 */
	prof_dump_open_file = prof_dump_open_file_error;
	expect_write_failure(0);

	/*
	 * When the n-th write fails, there shouldn't be any more write, and
	 * mallctl() should return failure.
	 */
	prof_dump_open_file = prof_dump_open_file_intercept;
	expect_write_failure(1); /* First write fails. */
	expect_write_failure(2); /* Second write fails. */

	dallocx(p, 0);

	prof_dump_open_file = open_file_orig;
	prof_dump_write_file = write_file_orig;
}
TEST_END

static int
prof_dump_open_maps_error(void) {
	return -1;
}

static bool started_piping_maps_file;

static ssize_t
prof_dump_write_maps_file_error(int fd, const void *s, size_t len) {
	/* The main dump doesn't contain any capital 'M'. */
	if (!started_piping_maps_file && strchr(s, 'M') != NULL) {
		started_piping_maps_file = true;
	}

	if (started_piping_maps_file) {
		return prof_dump_write_file_error(fd, s, len);
	} else {
		/* Return success when we haven't started piping maps. */
		return 0;
	}
}

static void
expect_maps_write_failure(int count) {
	int mfd = prof_dump_open_maps();
	if (mfd == -1) {
		/* No need to continue if we just can't find the maps file. */
		return;
	}
	close(mfd);
	started_piping_maps_file = false;
	expect_write_failure(count);
	expect_true(started_piping_maps_file, "Should start piping maps");
}

TEST_BEGIN(test_mdump_maps_error) {
	test_skip_if(!config_prof);
	test_skip_if(!config_debug);
	test_skip_if(prof_dump_open_maps == NULL);

	prof_dump_open_file_t  *open_file_orig = prof_dump_open_file;
	prof_dump_write_file_t *write_file_orig = prof_dump_write_file;
	prof_dump_open_maps_t  *open_maps_orig = prof_dump_open_maps;

	prof_dump_open_file = prof_dump_open_file_intercept;
	prof_dump_write_file = prof_dump_write_maps_file_error;

	void *p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	/*
	 * When opening the maps file fails, there shouldn't be any maps write,
	 * and mallctl() should return success.
	 */
	prof_dump_open_maps = prof_dump_open_maps_error;
	started_piping_maps_file = false;
	prof_dump_write_file_count = 0;
	expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&test_filename,
	                sizeof(test_filename)),
	    0, "mallctl should not fail in case of maps file opening failure");
	expect_false(started_piping_maps_file, "Shouldn't start piping maps");
	expect_d_eq(prof_dump_write_file_count, 0,
	    "Dumping stopped after a wrong number of writes");

	/*
	 * When the n-th maps write fails (given that we are able to find the
	 * maps file), there shouldn't be any more maps write, and mallctl()
	 * should return failure.
	 */
	prof_dump_open_maps = open_maps_orig;
	expect_maps_write_failure(1); /* First write fails. */
	expect_maps_write_failure(2); /* Second write fails. */

	dallocx(p, 0);

	prof_dump_open_file = open_file_orig;
	prof_dump_write_file = write_file_orig;
}
TEST_END

int
main(void) {
	return test(test_prof_parse_pid_namespace, test_mdump_normal,
	    test_mdump_output_error, test_mdump_maps_error);
}
