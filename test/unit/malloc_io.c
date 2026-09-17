#include "test/jemalloc_test.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif

TEST_BEGIN(test_malloc_strtoumax_no_endptr) {
	int err;

	set_errno(0);
	expect_ju_eq(malloc_strtoumax("0", NULL, 0), 0, "Unexpected result");
	err = get_errno();
	expect_d_eq(err, 0, "Unexpected failure");
}
TEST_END

TEST_BEGIN(test_malloc_strtoumax) {
	struct test_s {
		const char *input;
		const char *expected_remainder;
		int         base;
		int         expected_errno;
		const char *expected_errno_name;
		uintmax_t   expected_x;
	};
#define ERR(e) e, #e
#define KUMAX(x) ((uintmax_t)x##ULL)
#define KSMAX(x) ((uintmax_t)(intmax_t)x##LL)
	struct test_s tests[] = {{"0", "0", -1, ERR(EINVAL), UINTMAX_MAX},
	    {"0", "0", 1, ERR(EINVAL), UINTMAX_MAX},
	    {"0", "0", 37, ERR(EINVAL), UINTMAX_MAX},

	    {"", "", 0, ERR(EINVAL), UINTMAX_MAX},
	    {"+", "+", 0, ERR(EINVAL), UINTMAX_MAX},
	    {"++3", "++3", 0, ERR(EINVAL), UINTMAX_MAX},
	    {"-", "-", 0, ERR(EINVAL), UINTMAX_MAX},

	    {"42", "", 0, ERR(0), KUMAX(42)}, {"+42", "", 0, ERR(0), KUMAX(42)},
	    {"-42", "", 0, ERR(0), KSMAX(-42)},
	    {"042", "", 0, ERR(0), KUMAX(042)},
	    {"+042", "", 0, ERR(0), KUMAX(042)},
	    {"-042", "", 0, ERR(0), KSMAX(-042)},
	    {"0x42", "", 0, ERR(0), KUMAX(0x42)},
	    {"+0x42", "", 0, ERR(0), KUMAX(0x42)},
	    {"-0x42", "", 0, ERR(0), KSMAX(-0x42)},

	    {"0", "", 0, ERR(0), KUMAX(0)}, {"1", "", 0, ERR(0), KUMAX(1)},

	    {"42", "", 0, ERR(0), KUMAX(42)}, {" 42", "", 0, ERR(0), KUMAX(42)},
	    {"42 ", " ", 0, ERR(0), KUMAX(42)},
	    {"0x", "x", 0, ERR(0), KUMAX(0)},
	    {"42x", "x", 0, ERR(0), KUMAX(42)},

	    {"07", "", 0, ERR(0), KUMAX(7)}, {"010", "", 0, ERR(0), KUMAX(8)},
	    {"08", "8", 0, ERR(0), KUMAX(0)}, {"0_", "_", 0, ERR(0), KUMAX(0)},

	    {"0x", "x", 0, ERR(0), KUMAX(0)}, {"0X", "X", 0, ERR(0), KUMAX(0)},
	    {"0xg", "xg", 0, ERR(0), KUMAX(0)},
	    {"0XA", "", 0, ERR(0), KUMAX(10)},

	    {"010", "", 10, ERR(0), KUMAX(10)},
	    {"0x3", "x3", 10, ERR(0), KUMAX(0)},

	    {"12", "2", 2, ERR(0), KUMAX(1)}, {"78", "8", 8, ERR(0), KUMAX(7)},
	    {"9a", "a", 10, ERR(0), KUMAX(9)},
	    {"9A", "A", 10, ERR(0), KUMAX(9)},
	    {"fg", "g", 16, ERR(0), KUMAX(15)},
	    {"FG", "G", 16, ERR(0), KUMAX(15)},
	    {"0xfg", "g", 16, ERR(0), KUMAX(15)},
	    {"0XFG", "G", 16, ERR(0), KUMAX(15)},
	    {"z_", "_", 36, ERR(0), KUMAX(35)},
	    {"Z_", "_", 36, ERR(0), KUMAX(35)}};
#undef ERR
#undef KUMAX
#undef KSMAX
	unsigned i;

	for (i = 0; i < sizeof(tests) / sizeof(struct test_s); i++) {
		struct test_s *test = &tests[i];
		int            err;
		uintmax_t      result;
		char          *remainder;

		set_errno(0);
		result = malloc_strtoumax(test->input, &remainder, test->base);
		err = get_errno();
		expect_d_eq(err, test->expected_errno,
		    "Expected errno %s for \"%s\", base %d",
		    test->expected_errno_name, test->input, test->base);
		expect_str_eq(remainder, test->expected_remainder,
		    "Unexpected remainder for \"%s\", base %d", test->input,
		    test->base);
		if (err == 0) {
			expect_ju_eq(result, test->expected_x,
			    "Unexpected result for \"%s\", base %d",
			    test->input, test->base);
		}
	}
}
TEST_END

TEST_BEGIN(test_malloc_snprintf_truncated) {
#define BUFLEN 15
	char   buf[BUFLEN];
	size_t result;
	size_t len;
#define TEST(expected_str_untruncated, ...)                                    \
	do {                                                                   \
		result = malloc_snprintf(buf, len, __VA_ARGS__);               \
		expect_d_eq(strncmp(buf, expected_str_untruncated, len - 1),   \
		    0, "Unexpected string inequality (\"%s\" vs \"%s\")", buf, \
		    expected_str_untruncated);                                 \
		expect_zu_eq(result, strlen(expected_str_untruncated),         \
		    "Unexpected result");                                      \
	} while (0)

	for (len = 1; len < BUFLEN; len++) {
		TEST("012346789", "012346789");
		TEST("a0123b", "a%sb", "0123");
		TEST("a01234567", "a%s%s", "0123", "4567");
		TEST("a0123  ", "a%-6s", "0123");
		TEST("a  0123", "a%6s", "0123");
		TEST("a   012", "a%6.3s", "0123");
		TEST("a   012", "a%*.*s", 6, 3, "0123");
		TEST("a 123b", "a% db", 123);
		TEST("a123b", "a%-db", 123);
		TEST("a-123b", "a%-db", -123);
		TEST("a+123b", "a%+db", 123);
	}
#undef BUFLEN
#undef TEST
}
TEST_END

TEST_BEGIN(test_malloc_snprintf) {
#define BUFLEN 128
	char   buf[BUFLEN];
	size_t result;
#define TEST(expected_str, ...)                                                \
	do {                                                                   \
		result = malloc_snprintf(buf, sizeof(buf), __VA_ARGS__);       \
		expect_str_eq(buf, expected_str, "Unexpected output");         \
		expect_zu_eq(                                                  \
		    result, strlen(expected_str), "Unexpected result");        \
	} while (0)

	TEST("hello", "hello");

	TEST("50%, 100%", "50%%, %d%%", 100);

	TEST("a0123b", "a%sb", "0123");

	TEST("a 0123b", "a%5sb", "0123");
	TEST("a 0123b", "a%*sb", 5, "0123");

	TEST("a0123 b", "a%-5sb", "0123");
	TEST("a0123b", "a%*sb", -1, "0123");
	TEST("a0123 b", "a%*sb", -5, "0123");
	TEST("a0123 b", "a%-*sb", -5, "0123");

	TEST("a012b", "a%.3sb", "0123");
	TEST("a012b", "a%.*sb", 3, "0123");
	TEST("a0123b", "a%.*sb", -3, "0123");

	TEST("a  012b", "a%5.3sb", "0123");
	TEST("a  012b", "a%5.*sb", 3, "0123");
	TEST("a  012b", "a%*.3sb", 5, "0123");
	TEST("a  012b", "a%*.*sb", 5, 3, "0123");
	TEST("a 0123b", "a%*.*sb", 5, -3, "0123");

	TEST("_abcd_", "_%x_", 0xabcd);
	TEST("_0xabcd_", "_%#x_", 0xabcd);
	TEST("_1234_", "_%o_", 01234);
	TEST("_01234_", "_%#o_", 01234);
	TEST("_1234_", "_%u_", 1234);
	TEST("01234", "%05u", 1234);

	TEST("_1234_", "_%d_", 1234);
	TEST("_ 1234_", "_% d_", 1234);
	TEST("_+1234_", "_%+d_", 1234);
	TEST("_-1234_", "_%d_", -1234);
	TEST("_-1234_", "_% d_", -1234);
	TEST("_-1234_", "_%+d_", -1234);

	/*
	 * Morally, we should test these too, but 0-padded signed types are not
	 * yet supported.
	 *
	 * TEST("01234", "%05", 1234);
	 * TEST("-1234", "%05d", -1234);
	 * TEST("-01234", "%06d", -1234);
	*/

	TEST("_-1234_", "_%d_", -1234);
	TEST("_1234_", "_%d_", 1234);
	TEST("_-1234_", "_%i_", -1234);
	TEST("_1234_", "_%i_", 1234);
	TEST("_01234_", "_%#o_", 01234);
	TEST("_1234_", "_%u_", 1234);
	TEST("_0x1234abc_", "_%#x_", 0x1234abc);
	TEST("_0X1234ABC_", "_%#X_", 0x1234abc);
	TEST("_c_", "_%c_", 'c');
	TEST("_string_", "_%s_", "string");
	TEST("_0x42_", "_%p_", ((void *)0x42));

	TEST("_-1234_", "_%ld_", ((long)-1234));
	TEST("_1234_", "_%ld_", ((long)1234));
	TEST("_-1234_", "_%li_", ((long)-1234));
	TEST("_1234_", "_%li_", ((long)1234));
	TEST("_01234_", "_%#lo_", ((long)01234));
	TEST("_1234_", "_%lu_", ((long)1234));
	TEST("_0x1234abc_", "_%#lx_", ((long)0x1234abc));
	TEST("_0X1234ABC_", "_%#lX_", ((long)0x1234ABC));

	TEST("_-1234_", "_%lld_", ((long long)-1234));
	TEST("_1234_", "_%lld_", ((long long)1234));
	TEST("_-1234_", "_%lli_", ((long long)-1234));
	TEST("_1234_", "_%lli_", ((long long)1234));
	TEST("_01234_", "_%#llo_", ((long long)01234));
	TEST("_1234_", "_%llu_", ((long long)1234));
	TEST("_0x1234abc_", "_%#llx_", ((long long)0x1234abc));
	TEST("_0X1234ABC_", "_%#llX_", ((long long)0x1234ABC));

	TEST("_-1234_", "_%qd_", ((long long)-1234));
	TEST("_1234_", "_%qd_", ((long long)1234));
	TEST("_-1234_", "_%qi_", ((long long)-1234));
	TEST("_1234_", "_%qi_", ((long long)1234));
	TEST("_01234_", "_%#qo_", ((long long)01234));
	TEST("_1234_", "_%qu_", ((long long)1234));
	TEST("_0x1234abc_", "_%#qx_", ((long long)0x1234abc));
	TEST("_0X1234ABC_", "_%#qX_", ((long long)0x1234ABC));

	TEST("_-1234_", "_%jd_", ((intmax_t)-1234));
	TEST("_1234_", "_%jd_", ((intmax_t)1234));
	TEST("_-1234_", "_%ji_", ((intmax_t)-1234));
	TEST("_1234_", "_%ji_", ((intmax_t)1234));
	TEST("_01234_", "_%#jo_", ((intmax_t)01234));
	TEST("_1234_", "_%ju_", ((intmax_t)1234));
	TEST("_0x1234abc_", "_%#jx_", ((intmax_t)0x1234abc));
	TEST("_0X1234ABC_", "_%#jX_", ((intmax_t)0x1234ABC));

	TEST("_1234_", "_%td_", ((ptrdiff_t)1234));
	TEST("_-1234_", "_%td_", ((ptrdiff_t)-1234));
	TEST("_1234_", "_%ti_", ((ptrdiff_t)1234));
	TEST("_-1234_", "_%ti_", ((ptrdiff_t)-1234));

	TEST("_-1234_", "_%zd_", ((ssize_t)-1234));
	TEST("_1234_", "_%zd_", ((ssize_t)1234));
	TEST("_-1234_", "_%zi_", ((ssize_t)-1234));
	TEST("_1234_", "_%zi_", ((ssize_t)1234));
	TEST("_01234_", "_%#zo_", ((ssize_t)01234));
	TEST("_1234_", "_%zu_", ((ssize_t)1234));
	TEST("_0x1234abc_", "_%#zx_", ((ssize_t)0x1234abc));
	TEST("_0X1234ABC_", "_%#zX_", ((ssize_t)0x1234ABC));
#undef BUFLEN
}
TEST_END

TEST_BEGIN(test_malloc_snprintf_zero_size) {
	char   buf[8];
	size_t result;

	/*
	 * malloc_snprintf with size==0 should not write anything but should
	 * return the length that would have been written.  A previous bug
	 * caused an out-of-bounds write via str[size - 1] when size was 0.
	 */
	memset(buf, 'X', sizeof(buf));
	result = malloc_snprintf(buf, 0, "%s", "hello");
	expect_zu_eq(result, 5, "Expected length 5 for \"hello\"");
	/* buf should be untouched. */
	expect_c_eq(buf[0], 'X', "Buffer should not have been modified");
}
TEST_END

/*
 * Exercised via malloc_open()/malloc_close() (existing, already
 * cross-platform malloc_io.h wrappers) rather than an anonymous pipe: this
 * mirrors how malloc_write_fd()/malloc_read_fd() are actually used in
 * production (pages.c, prof_stack_range.c both read/write real files;
 * nothing in jemalloc ever pipes through them). Written and read back via
 * separate malloc_open() calls rather than a shared fd + seek-to-0, since
 * malloc_lseek() (like the os_file_lseek() removed earlier) has no
 * production caller either.
 *
 * The file itself is created via fopen()/fclose(), not malloc_open(): the
 * latter is never called with O_CREAT in production (only O_RDONLY, on
 * files that already exist), and its 2-arg signature has no mode_t
 * parameter to pass along if it were -- calling the underlying variadic
 * open() with O_CREAT but no mode gives the new file garbage permissions.
 */
static const char *test_io_filename = "malloc_io_test_file.tmp";

static void
create_empty_test_io_file(void) {
	FILE *fp = fopen(test_io_filename, "wb");
	assert_ptr_not_null(fp, "Unexpected fopen() failure");
	fclose(fp);
}

TEST_BEGIN(test_malloc_write_read_fd_roundtrip) {
	create_empty_test_io_file();
	int fd = malloc_open(test_io_filename, O_WRONLY | O_BINARY);
	assert_d_ne(fd, -1, "Unexpected malloc_open() failure");

	static const char data[] =
	    "malloc_write_fd()/malloc_read_fd() round-trip test payload.";

	ssize_t written = malloc_write_fd(fd, data, sizeof(data));
	expect_zd_eq(written, (ssize_t)sizeof(data),
	    "malloc_write_fd() should write the full buffer");
	malloc_close(fd);

	fd = malloc_open(test_io_filename, O_RDONLY | O_BINARY);
	assert_d_ne(fd, -1, "Unexpected malloc_open() failure");

	char buf[sizeof(data)];
	memset(buf, 0, sizeof(buf));
	ssize_t nread = malloc_read_fd(fd, buf, sizeof(buf));
	expect_zd_eq(nread, (ssize_t)sizeof(data),
	    "malloc_read_fd() should read back everything that was written");
	expect_d_eq(memcmp(buf, data, sizeof(data)), 0,
	    "Round-tripped data should be unchanged");

	malloc_close(fd);
	remove(test_io_filename);
}
TEST_END

TEST_BEGIN(test_malloc_read_fd_eof) {
	create_empty_test_io_file();
	int fd = malloc_open(test_io_filename, O_RDONLY | O_BINARY);
	assert_d_ne(fd, -1, "Unexpected malloc_open() failure");

	char buf[8];
	ssize_t nread = malloc_read_fd(fd, buf, sizeof(buf));
	expect_zd_eq(nread, 0,
	    "malloc_read_fd() should report an empty file as a zero-length "
	    "read (EOF)");

	malloc_close(fd);
	remove(test_io_filename);
}
TEST_END

TEST_BEGIN(test_malloc_write_read_fd_accumulate) {
	create_empty_test_io_file();
	int fd = malloc_open(test_io_filename, O_WRONLY | O_BINARY);
	assert_d_ne(fd, -1, "Unexpected malloc_open() failure");

	static const char part1[] = "0123456789";
	static const char part2[] = "abcdefghij";
	size_t full_len = sizeof(part1) - 1 + sizeof(part2) - 1;

	/*
	 * Two separate writes, read back with a single malloc_read_fd() call
	 * for the combined length.  This verifies that data written in
	 * separate calls comes back in order and undamaged; it's also the
	 * scenario malloc_read_fd()'s accumulate-until-count-satisfied loop
	 * exists to handle, on platforms/fds where one read() doesn't return
	 * everything at once.
	 */
	expect_zd_eq(malloc_write_fd(fd, part1, sizeof(part1) - 1),
	    (ssize_t)sizeof(part1) - 1, "Unexpected short write");
	expect_zd_eq(malloc_write_fd(fd, part2, sizeof(part2) - 1),
	    (ssize_t)sizeof(part2) - 1, "Unexpected short write");
	malloc_close(fd);

	fd = malloc_open(test_io_filename, O_RDONLY | O_BINARY);
	assert_d_ne(fd, -1, "Unexpected malloc_open() failure");

	char buf[64];
	memset(buf, 0, sizeof(buf));
	ssize_t nread = malloc_read_fd(fd, buf, full_len);
	expect_zd_eq(nread, (ssize_t)full_len,
	    "malloc_read_fd() should return the full combined length");
	expect_d_eq(memcmp(buf, part1, sizeof(part1) - 1), 0,
	    "Unexpected content for the first part");
	expect_d_eq(memcmp(buf + sizeof(part1) - 1, part2, sizeof(part2) - 1),
	    0, "Unexpected content for the second part");

	malloc_close(fd);
	remove(test_io_filename);
}
TEST_END

TEST_BEGIN(test_malloc_write_read_fd_bad_fd) {
#ifndef _WIN32
	/*
	 * -1 is never a valid fd, on any platform; both wrappers should
	 * propagate the error rather than loop forever. Not run on Windows:
	 * the MSVC CRT's _read()/_write() route an invalid fd through
	 * _invalid_parameter_handler(), which can raise a debug assertion
	 * instead of returning -1/EBADF like POSIX guarantees. So
	 * "negative return for a bad fd" isn't actually a portable contract
	 * to test against on that CRT.
	 */
	expect_zd_lt(
	    malloc_write_fd(-1, "x", 1), (ssize_t)0, "Expected write error");
	char buf[1];
	expect_zd_lt(malloc_read_fd(-1, buf, sizeof(buf)), (ssize_t)0,
	    "Expected read error");
#else
	test_skip("Invalid-fd handling is not a portable contract on the "
	    "MSVC CRT; see comment above");
#endif
}
TEST_END

int
main(void) {
	return test(test_malloc_strtoumax_no_endptr, test_malloc_strtoumax,
	    test_malloc_snprintf_truncated, test_malloc_snprintf,
	    test_malloc_snprintf_zero_size, test_malloc_write_read_fd_roundtrip,
	    test_malloc_read_fd_eof, test_malloc_write_read_fd_accumulate,
	    test_malloc_write_read_fd_bad_fd);
}
