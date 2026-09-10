#include "test/jemalloc_test.h"

#include "jemalloc/internal/safety_check.h"

bool fake_abort_called;
void
fake_abort(const char *message) {
	(void)message;
	fake_abort_called = true;
}

#define SMALL_SIZE1 SC_SMALL_MAXCLASS
#define SMALL_SIZE2 (SC_SMALL_MAXCLASS / 2)

#define LARGE_SIZE1 SC_LARGE_MINCLASS
#define LARGE_SIZE2 (LARGE_SIZE1 * 2)

static void *
test_invalid_size_pre(size_t sz) {
	test_hooks_safety_check_abort = &fake_abort;

	fake_abort_called = false;
	void *ptr = malloc(sz);
	assert_ptr_not_null(ptr, "Unexpected failure");

	return ptr;
}

static void
test_invalid_size_post(void) {
	expect_true(fake_abort_called, "Safety check didn't fire");
	test_hooks_safety_check_abort = NULL;
}

TEST_BEGIN(test_invalid_size_sdallocx) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	sdallocx(ptr, SMALL_SIZE2, 0);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	sdallocx(ptr, LARGE_SIZE2, 0);
	test_invalid_size_post();
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_nonzero_flag) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	sdallocx(ptr, SMALL_SIZE2, MALLOCX_TCACHE_NONE);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	sdallocx(ptr, LARGE_SIZE2, MALLOCX_TCACHE_NONE);
	test_invalid_size_post();
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_noflags) {
	test_skip_if(!config_opt_size_checks);

	void *ptr = test_invalid_size_pre(SMALL_SIZE1);
	je_sdallocx_noflags(ptr, SMALL_SIZE2);
	test_invalid_size_post();

	ptr = test_invalid_size_pre(LARGE_SIZE1);
	je_sdallocx_noflags(ptr, LARGE_SIZE2);
	test_invalid_size_post();
}
TEST_END

TEST_BEGIN(test_valid_zero_size_aligned_sdallocx) {
	test_skip_if(!config_opt_size_checks);

	const size_t alignment = PAGE;
	void        *ptr = aligned_alloc(alignment, 0);
	assert_ptr_not_null(ptr, "Unexpected aligned_alloc() failure");

	fake_abort_called = false;
	test_hooks_safety_check_abort = &fake_abort;
	sdallocx(ptr, 0, MALLOCX_ALIGN(alignment));
	bool failed = fake_abort_called;
	test_hooks_safety_check_abort = NULL;

	if (failed) {
		/* The intercepted mismatch path deliberately did not free ptr. */
		free(ptr);
	}
	expect_false(
	    failed, "Valid zero-sized aligned deallocation failed size check");
}
TEST_END

int
main(void) {
	return test(test_invalid_size_sdallocx,
	    test_invalid_size_sdallocx_nonzero_flag,
	    test_invalid_size_sdallocx_noflags,
	    test_valid_zero_size_aligned_sdallocx);
}
