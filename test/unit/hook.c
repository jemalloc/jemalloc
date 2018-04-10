#include "test/jemalloc_test.h"

#include "jemalloc/internal/hook.h"

static void *arg_extra;
static int arg_type;
static void *arg_result;
static void *arg_address;
static size_t arg_old_usize;
static size_t arg_new_usize;
static uintptr_t arg_result_raw;
static uintptr_t arg_args_raw[4];

static int call_count = 0;

static void
reset_args() {
	arg_extra = NULL;
	arg_type = 12345;
	arg_result = NULL;
	arg_address = NULL;
	arg_old_usize = 0;
	arg_new_usize = 0;
	arg_result_raw = 0;
	memset(arg_args_raw, 77, sizeof(arg_args_raw));
}

static void
set_args_raw(uintptr_t *args_raw, int nargs) {
	memcpy(arg_args_raw, args_raw, sizeof(uintptr_t) * nargs);
}

static void
assert_args_raw(uintptr_t *args_raw_expected, int nargs) {
	int cmp = memcmp(args_raw_expected, arg_args_raw,
	    sizeof(uintptr_t) * nargs);
	assert_d_eq(cmp, 0, "Raw args mismatch");
}

static void
test_alloc_hook(void *extra, hook_alloc_t type, void *result,
    uintptr_t result_raw, uintptr_t args_raw[3]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_result = result;
	arg_result_raw = result_raw;
	set_args_raw(args_raw, 3);
}

static void
test_dalloc_hook(void *extra, hook_dalloc_t type, void *address,
    uintptr_t args_raw[3]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_address = address;
	set_args_raw(args_raw, 3);
}

static void
test_expand_hook(void *extra, hook_expand_t type, void *address,
    size_t old_usize, size_t new_usize, uintptr_t result_raw,
    uintptr_t args_raw[4]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_address = address;
	arg_old_usize = old_usize;
	arg_new_usize = new_usize;
	arg_result_raw = result_raw;
	set_args_raw(args_raw, 4);
}

TEST_BEGIN(test_hooks_basic) {
	/* Just verify that the record their arguments correctly. */
	hooks_t hooks = {
		&test_alloc_hook, &test_dalloc_hook, &test_expand_hook};
	void *handle = hook_install(TSDN_NULL, &hooks, (void *)111);
	uintptr_t args_raw[4] = {10, 20, 30, 40};

	/* Alloc */
	reset_args();
	hook_invoke_alloc(hook_alloc_posix_memalign, (void *)222, 333,
	    args_raw);
	assert_ptr_eq(arg_extra, (void *)111, "Passed wrong user pointer");
	assert_d_eq((int)hook_alloc_posix_memalign, arg_type,
	    "Passed wrong alloc type");
	assert_ptr_eq((void *)222, arg_result, "Passed wrong result address");
	assert_u64_eq(333, arg_result_raw, "Passed wrong result");
	assert_args_raw(args_raw, 3);

	/* Dalloc */
	reset_args();
	hook_invoke_dalloc(hook_dalloc_sdallocx, (void *)222, args_raw);
	assert_d_eq((int)hook_dalloc_sdallocx, arg_type,
	    "Passed wrong dalloc type");
	assert_ptr_eq((void *)111, arg_extra, "Passed wrong user pointer");
	assert_ptr_eq((void *)222, arg_address, "Passed wrong address");
	assert_args_raw(args_raw, 3);

	/* Expand */
	reset_args();
	hook_invoke_expand(hook_expand_xallocx, (void *)222, 333, 444, 555,
	    args_raw);
	assert_d_eq((int)hook_expand_xallocx, arg_type,
	    "Passed wrong expand type");
	assert_ptr_eq((void *)111, arg_extra, "Passed wrong user pointer");
	assert_ptr_eq((void *)222, arg_address, "Passed wrong address");
	assert_zu_eq(333, arg_old_usize, "Passed wrong old usize");
	assert_zu_eq(444, arg_new_usize, "Passed wrong new usize");
	assert_zu_eq(555, arg_result_raw, "Passed wrong result");
	assert_args_raw(args_raw, 4);

	hook_remove(TSDN_NULL, handle);
}
TEST_END

TEST_BEGIN(test_hooks_null) {
	/* Null hooks should be ignored, not crash. */
	hooks_t hooks1 = {NULL, NULL, NULL};
	hooks_t hooks2 = {&test_alloc_hook, NULL, NULL};
	hooks_t hooks3 = {NULL, &test_dalloc_hook, NULL};
	hooks_t hooks4 = {NULL, NULL, &test_expand_hook};

	void *handle1 = hook_install(TSDN_NULL, &hooks1, NULL);
	void *handle2 = hook_install(TSDN_NULL, &hooks2, NULL);
	void *handle3 = hook_install(TSDN_NULL, &hooks3, NULL);
	void *handle4 = hook_install(TSDN_NULL, &hooks4, NULL);

	assert_ptr_ne(handle1, NULL, "Hook installation failed");
	assert_ptr_ne(handle2, NULL, "Hook installation failed");
	assert_ptr_ne(handle3, NULL, "Hook installation failed");
	assert_ptr_ne(handle4, NULL, "Hook installation failed");

	uintptr_t args_raw[4] = {10, 20, 30, 40};

	call_count = 0;
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, args_raw);
	assert_d_eq(call_count, 1, "Called wrong number of times");

	call_count = 0;
	hook_invoke_dalloc(hook_dalloc_free, NULL, args_raw);
	assert_d_eq(call_count, 1, "Called wrong number of times");

	call_count = 0;
	hook_invoke_expand(hook_expand_realloc, NULL, 0, 0, 0, args_raw);
	assert_d_eq(call_count, 1, "Called wrong number of times");

	hook_remove(TSDN_NULL, handle1);
	hook_remove(TSDN_NULL, handle2);
	hook_remove(TSDN_NULL, handle3);
	hook_remove(TSDN_NULL, handle4);
}
TEST_END

TEST_BEGIN(test_hooks_remove) {
	hooks_t hooks = {&test_alloc_hook, NULL, NULL};
	void *handle = hook_install(TSDN_NULL, &hooks, NULL);
	assert_ptr_ne(handle, NULL, "Hook installation failed");
	call_count = 0;
	uintptr_t args_raw[4] = {10, 20, 30, 40};
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, args_raw);
	assert_d_eq(call_count, 1, "Hook not invoked");

	call_count = 0;
	hook_remove(TSDN_NULL, handle);
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, NULL);
	assert_d_eq(call_count, 0, "Hook invoked after removal");

}
TEST_END

int
main(void) {
	return test(
	    test_hooks_basic,
	    test_hooks_null,
	    test_hooks_remove);
}
