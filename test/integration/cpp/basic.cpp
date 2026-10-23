#include <new>
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

TEST_BEGIN(test_aligned_sized_delete) {
#if __cpp_aligned_new >= 201606 && __cpp_sized_deallocation >= 201309
	const size_t alignments[] = {1, 64, 256, 4096, ZU(1) << 20};
	const size_t sizes[] = {1, 42, 4096, 65536};
	for (size_t alignment : alignments) {
		auto al = static_cast<std::align_val_t>(alignment);
		for (size_t size : sizes) {
			void *ptr = ::operator new(size, al);
			expect_ptr_not_null(
			    ptr, "Unexpected aligned new failure");
			expect_zu_eq((uintptr_t)ptr & (alignment - 1), 0,
			    "Unexpected allocation alignment");
			::operator delete(ptr, size, al);

			ptr = ::operator new[](size, al);
			expect_ptr_not_null(
			    ptr, "Unexpected aligned array new failure");
			expect_zu_eq((uintptr_t)ptr & (alignment - 1), 0,
			    "Unexpected allocation alignment");
			::operator delete[](ptr, size, al);
		}
		::operator delete(nullptr, 0, al);
		::operator delete[](nullptr, 0, al);
	}
#else
	test_skip("Aligned sized deallocation is not enabled");
#endif
}
TEST_END

int
main() {
	return test(test_basic, test_aligned_sized_delete);
}
