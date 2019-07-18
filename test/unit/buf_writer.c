#include "test/jemalloc_test.h"

#define TEST_BUF_SIZE 16
#define UNIT_MAX (TEST_BUF_SIZE * 3)

static size_t test_write_len;
static char test_buf[TEST_BUF_SIZE];
static size_t arg_store;

static void test_write_cb(void *cbopaque, const char *s) {
	size_t prev_test_write_len = test_write_len;
	test_write_len += strlen(s); /* only increase the length */
	arg_store = *(size_t *)cbopaque; /* only pass along the argument */
	assert_zu_le(prev_test_write_len, test_write_len,
	    "Test write overflowed");
}

TEST_BEGIN(test_buf_write) {
	char s[UNIT_MAX + 1];
	size_t unit, n_unit, remain, i;
	size_t arg = 1;
	buf_writer_arg_t test_buf_arg =
	    {test_write_cb, &arg, test_buf, TEST_BUF_SIZE - 1, 0};
	memset(s, 'a', UNIT_MAX);
	for (unit = UNIT_MAX; /* include 0 for edge case */; --unit) {
		/* unit keeps decreasing, so strlen(s) is always unit. */
		s[unit] = '\0';
		for (n_unit = 1; n_unit <= 3; ++n_unit) {
			test_write_len = 0;
			remain = 0;
			for (i = 1; i <= n_unit; ++i) {
				/* A toy pseudo-random number generator */
				arg = (arg * 5 + 7) % 8;
				buffered_write_cb(&test_buf_arg, s);
				remain += unit;
				if (remain > test_buf_arg.buf_size) {
					/* Flushes should have happened. */
					assert_zu_eq(arg_store, arg, "Call back"
					    " argument did not get through");
					remain %= test_buf_arg.buf_size;
					if (remain == 0) {
						/* Last flush should be lazy. */
						remain += test_buf_arg.buf_size;
					}
				}
				assert_zu_eq(test_write_len + remain, i * unit,
				    "Incorrect length after writing %zu strings"
				    " of length %zu", i, unit);
			}
			buf_writer_flush(&test_buf_arg);
			assert_zu_eq(test_write_len, n_unit * unit,
			    "Incorrect length after flushing at the end of"
			    " writing %zu strings of length %zu", n_unit, unit);
		}
		if (unit == 0) {
			break;
		}
	}
}
TEST_END

int
main(void) {
	return test(test_buf_write);
}
