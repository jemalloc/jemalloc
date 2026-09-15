#include <memory>

#include "test/jemalloc_test.h"

static_assert(JEMALLOC_INFALLIBLE_NEW == 0);

TEST_BEGIN(test_failing_alloc) {
	bool saw_exception = false;
	try {
		/* Too big of an allocation to succeed. */
		void *volatile ptr = ::operator new((size_t)-1);
		(void)ptr;
	} catch (...) {
		saw_exception = true;
	}
	expect_true(saw_exception, "Didn't get a failure");
}
TEST_END

#if __cpp_aligned_new >= 201606
static unsigned new_handler_calls;

static void
new_handler_once(void) {
	/* Let the retry run once, then give up. */
	if (++new_handler_calls > 1) {
		std::set_new_handler(nullptr);
	}
}

TEST_BEGIN(test_failing_aligned_alloc_new_handler) {
	/*
	 * The top bit of size_t exceeds SC_LARGE_MAXCLASS on every platform,
	 * so no allocation can satisfy this alignment and the retry after the
	 * new_handler must fail too, rather than returning an unaligned
	 * pointer.
	 */
	const std::size_t alignment = (SIZE_MAX >> 1) + 1;

	new_handler_calls = 0;
	std::set_new_handler(new_handler_once);
	bool saw_exception = false;
	try {
		void *volatile ptr = ::operator new(
		    16, std::align_val_t(alignment));
		(void)ptr;
	} catch (const std::bad_alloc &) {
		saw_exception = true;
	}
	std::set_new_handler(nullptr);
	expect_true(saw_exception, "Aligned new should have failed");
	expect_u_eq(new_handler_calls, 2, "Unexpected new_handler calls");

	new_handler_calls = 0;
	std::set_new_handler(new_handler_once);
	void *volatile ptr = ::operator new(
	    16, std::align_val_t(alignment), std::nothrow);
	std::set_new_handler(nullptr);
	expect_ptr_null(ptr, "Aligned nothrow new should have failed");
	expect_u_eq(new_handler_calls, 2, "Unexpected new_handler calls");
}
TEST_END
#endif

int
main(void) {
#if __cpp_aligned_new >= 201606
	return test(test_failing_alloc, test_failing_aligned_alloc_new_handler);
#else
	return test(test_failing_alloc);
#endif
}
