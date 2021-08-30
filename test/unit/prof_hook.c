#include "test/jemalloc_test.h"

bool mock_bt_hook_called = false;

void
mock_bt_hook(void **vec, unsigned *len, unsigned max_len) {
	*len = max_len;
	for (unsigned i = 0; i < max_len; ++i) {
		vec[i] = (void *)((uintptr_t)i);
	}
	mock_bt_hook_called = true;
}

TEST_BEGIN(test_prof_backtrace_hook) {

	test_skip_if(!config_prof);

	mock_bt_hook_called = false;

	void *p0 = mallocx(1, 0);
	assert_ptr_not_null(p0, "Failed to allocate");

	expect_false(mock_bt_hook_called, "Called mock hook before it's set");

	prof_backtrace_hook_t null_hook = NULL;
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    NULL, 0, (void *)&null_hook,  sizeof(null_hook)),
		EINVAL, "Incorrectly allowed NULL backtrace hook");

	prof_backtrace_hook_t default_hook;
	size_t default_hook_sz = sizeof(prof_backtrace_hook_t);
	prof_backtrace_hook_t hook = &mock_bt_hook;
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&default_hook, &default_hook_sz, (void *)&hook,
	    sizeof(hook)), 0, "Unexpected mallctl failure setting hook");

	void *p1 = mallocx(1, 0);
	assert_ptr_not_null(p1, "Failed to allocate");

	expect_true(mock_bt_hook_called, "Didn't call mock hook");

	prof_backtrace_hook_t current_hook;
	size_t current_hook_sz = sizeof(prof_backtrace_hook_t);
	expect_d_eq(mallctl("experimental.hooks.prof_backtrace",
	    (void *)&current_hook, &current_hook_sz, (void *)&default_hook,
	    sizeof(default_hook)), 0,
	    "Unexpected mallctl failure resetting hook to default");

	expect_ptr_eq(current_hook, hook,
	    "Hook returned by mallctl is not equal to mock hook");

	dallocx(p1, 0);
	dallocx(p0, 0);
}
TEST_END

int
main(void) {
	return test(
	    test_prof_backtrace_hook);
}
