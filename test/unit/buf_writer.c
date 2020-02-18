#include "test/jemalloc_test.h"

#include "jemalloc/internal/buf_writer.h"

#define TEST_BUF_SIZE 16
#define UNIT_MAX (TEST_BUF_SIZE * 3)

static size_t test_write_len;
static char test_buf[TEST_BUF_SIZE];
static uint64_t arg;
static uint64_t arg_store;

static void test_write_cb(void *cbopaque, const char *s) {
	size_t prev_test_write_len = test_write_len;
	test_write_len += strlen(s); /* only increase the length */
	arg_store = *(uint64_t *)cbopaque; /* only pass along the argument */
	expect_zu_le(prev_test_write_len, test_write_len,
	    "Test write overflowed");
}

static void test_buf_writer_body(tsdn_t *tsdn, buf_writer_t *buf_writer) {
	char s[UNIT_MAX + 1];
	size_t n_unit, remain, i;
	ssize_t unit;
	expect_ptr_not_null(buf_writer->buf, "Buffer is null");
	write_cb_t *write_cb = buf_writer_get_write_cb(buf_writer);
	void *cbopaque = buf_writer_get_cbopaque(buf_writer);

	memset(s, 'a', UNIT_MAX);
	arg = 4; /* Starting value of random argument. */
	arg_store = arg;
	for (unit = UNIT_MAX; unit >= 0; --unit) {
		/* unit keeps decreasing, so strlen(s) is always unit. */
		s[unit] = '\0';
		for (n_unit = 1; n_unit <= 3; ++n_unit) {
			test_write_len = 0;
			remain = 0;
			for (i = 1; i <= n_unit; ++i) {
				arg = prng_lg_range_u64(&arg, 64);
				write_cb(cbopaque, s);
				remain += unit;
				if (remain > buf_writer->buf_size) {
					/* Flushes should have happened. */
					expect_u64_eq(arg_store, arg, "Call "
					    "back argument didn't get through");
					remain %= buf_writer->buf_size;
					if (remain == 0) {
						/* Last flush should be lazy. */
						remain += buf_writer->buf_size;
					}
				}
				expect_zu_eq(test_write_len + remain, i * unit,
				    "Incorrect length after writing %zu strings"
				    " of length %zu", i, unit);
			}
			buf_writer_flush(buf_writer);
			expect_zu_eq(test_write_len, n_unit * unit,
			    "Incorrect length after flushing at the end of"
			    " writing %zu strings of length %zu", n_unit, unit);
		}
	}
	buf_writer_terminate(tsdn, buf_writer);
}

TEST_BEGIN(test_buf_write_static) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	expect_false(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    test_buf, TEST_BUF_SIZE),
	    "buf_writer_init() should not encounter error on static buffer");
	test_buf_writer_body(tsdn, &buf_writer);
}
TEST_END

TEST_BEGIN(test_buf_write_dynamic) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	expect_false(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    NULL, TEST_BUF_SIZE), "buf_writer_init() should not OOM");
	test_buf_writer_body(tsdn, &buf_writer);
}
TEST_END

TEST_BEGIN(test_buf_write_oom) {
	buf_writer_t buf_writer;
	tsdn_t *tsdn = tsdn_fetch();
	expect_true(buf_writer_init(tsdn, &buf_writer, test_write_cb, &arg,
	    NULL, SC_LARGE_MAXCLASS + 1), "buf_writer_init() should OOM");
	expect_ptr_null(buf_writer.buf, "Buffer should be null");
	write_cb_t *write_cb = buf_writer_get_write_cb(&buf_writer);
	expect_ptr_eq(write_cb, test_write_cb, "Should use test_write_cb");
	void *cbopaque = buf_writer_get_cbopaque(&buf_writer);
	expect_ptr_eq(cbopaque, &arg, "Should use arg");

	char s[UNIT_MAX + 1];
	size_t n_unit, i;
	ssize_t unit;

	memset(s, 'a', UNIT_MAX);
	arg = 4; /* Starting value of random argument. */
	arg_store = arg;
	for (unit = UNIT_MAX; unit >= 0; unit -= UNIT_MAX / 4) {
		/* unit keeps decreasing, so strlen(s) is always unit. */
		s[unit] = '\0';
		for (n_unit = 1; n_unit <= 3; ++n_unit) {
			test_write_len = 0;
			for (i = 1; i <= n_unit; ++i) {
				arg = prng_lg_range_u64(&arg, 64);
				write_cb(cbopaque, s);
				expect_u64_eq(arg_store, arg,
				    "Call back argument didn't get through");
				expect_zu_eq(test_write_len, i * unit,
				    "Incorrect length after writing %zu strings"
				    " of length %zu", i, unit);
			}
			buf_writer_flush(&buf_writer);
			expect_zu_eq(test_write_len, n_unit * unit,
			    "Incorrect length after flushing at the end of"
			    " writing %zu strings of length %zu", n_unit, unit);
		}
	}
	buf_writer_terminate(tsdn, &buf_writer);
}
TEST_END

int
main(void) {
	return test(
	    test_buf_write_static,
	    test_buf_write_dynamic,
	    test_buf_write_oom);
}
