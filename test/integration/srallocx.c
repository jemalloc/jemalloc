#include "test/jemalloc_test.h"


/* Generate random printable ASCII char */
static char random_char(size_t pos) {
	return (char) ('!' + ((pos * 17) % ('~' - '!' + 1)));
}


static void test_for_size(size_t first_alloc_size, size_t realloc_size) {
	void *p;
	void *q;

	size_t used_size = first_alloc_size / 2;

	p = mallocx(first_alloc_size, 0);
	assert_ptr_not_null(p, "mallocx() unexpectedly failed");

	for (size_t i = 0; i < first_alloc_size; ++i) {
		((char*) p)[i] = random_char(i);
	}

	q = srallocx(p, first_alloc_size, used_size, realloc_size, 0);
	assert_ptr_not_null(q, "srallocx() unexpectedly failed");
	assert_ptr_ne(p, q, "should reallocate with changed addr for following checks");

	for (size_t i = 0; i < used_size; ++i) {
		assert_c_eq(random_char(i), ((char*) q)[i], "data is copied in srallocx()");
	}

/*
 * not running this part of test under valgrind
 * because if srallocx works properly, this code reads
 * uninitialized memory
 */
#ifndef JEMALLOC_VALGRIND
	for (size_t i = used_size; ; ++i) {
		if (i == first_alloc_size) {
			assert_not_reached("either all data copied"
				" or random memory contained expected sequence");
		}

		if (random_char(i) != ((char*) q)[i]) {
			// found byte that is not copied by srallocx
			break;
		}
	}
#endif

	sdallocx(q, realloc_size, 0);
}


TEST_BEGIN(test_basic)
{
	test_for_size(100, 1000);
	test_for_size(100, 10000000);
	test_for_size(1000000, 2000000);
}
TEST_END

int
main(void)
{
	return (test(
		test_basic));
}
