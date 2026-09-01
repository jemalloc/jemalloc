#include <type_traits>

#include "test/jemalloc_test.h"

#if JEMALLOC_INFALLIBLE_NEW != 0 && JEMALLOC_INFALLIBLE_NEW != 1
#  error "JEMALLOC_INFALLIBLE_NEW must be 0 or 1"
#endif

using InfallibleNewConfig =
    std::bool_constant<JEMALLOC_INFALLIBLE_NEW != 0>;

void infallible_new_noexcept_probe()
    noexcept(JEMALLOC_INFALLIBLE_NEW);

static_assert(noexcept(infallible_new_noexcept_probe()) ==
    InfallibleNewConfig::value);

TEST_BEGIN(test_basic) {
	auto foo = new long(4);
	expect_ptr_not_null(foo, "Unexpected new[] failure");
	delete foo;
	// Test nullptr handling.
	foo = nullptr;
	delete foo;

	auto bar = new long;
	expect_ptr_not_null(bar, "Unexpected new failure");
	delete bar;
	// Test nullptr handling.
	bar = nullptr;
	delete bar;
}
TEST_END

int
main() {
	return test(test_basic);
}
