#include <memory>
#include "test/jemalloc_test.h"

TEST_BEGIN(test_basic)
{
	auto foo = new long(4);
	assert_ptr_not_null(foo, "Unexpected new[] failure");
	delete foo;
}
TEST_END

int
main()
{

	return (test(
	    test_basic));
}
