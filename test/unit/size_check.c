#include "test/jemalloc_test.h"

#include "jemalloc/internal/safety_check.h"

bool fake_abort_called;
void fake_abort(const char *message) {
	(void)message;
	fake_abort_called = true;
}

#define SIZE1 SC_SMALL_MAXCLASS
#define SIZE2 (SC_SMALL_MAXCLASS / 2)

TEST_BEGIN(test_invalid_size_sdallocx) {
	test_skip_if(!config_opt_size_checks);
	safety_check_set_abort(&fake_abort);

	fake_abort_called = false;
	void *ptr = malloc(SIZE1);
	assert_ptr_not_null(ptr, "Unexpected failure");
	sdallocx(ptr, SIZE2, 0);
	expect_true(fake_abort_called, "Safety check didn't fire");

	safety_check_set_abort(NULL);
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_nonzero_flag) {
	test_skip_if(!config_opt_size_checks);
	safety_check_set_abort(&fake_abort);

	fake_abort_called = false;
	void *ptr = malloc(SIZE1);
	assert_ptr_not_null(ptr, "Unexpected failure");
	sdallocx(ptr, SIZE2, MALLOCX_TCACHE_NONE);
	expect_true(fake_abort_called, "Safety check didn't fire");

	safety_check_set_abort(NULL);
}
TEST_END

TEST_BEGIN(test_invalid_size_sdallocx_noflags) {
	test_skip_if(!config_opt_size_checks);
	safety_check_set_abort(&fake_abort);

	fake_abort_called = false;
	void *ptr = malloc(SIZE1);
	assert_ptr_not_null(ptr, "Unexpected failure");
	je_sdallocx_noflags(ptr, SIZE2);
	expect_true(fake_abort_called, "Safety check didn't fire");

	safety_check_set_abort(NULL);
}
TEST_END

int
main(void) {
	return test(
	    test_invalid_size_sdallocx,
	    test_invalid_size_sdallocx_nonzero_flag,
	    test_invalid_size_sdallocx_noflags);
}
