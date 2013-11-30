#include "test/jemalloc_test.h"

#define	JEMALLOC_TEST_EXIT_FAIL	1
#define	JEMALLOC_TEST_EXIT_SKIP	2

JEMALLOC_ATTR(format(printf, 1, 2))
void
test_fail(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
	exit(JEMALLOC_TEST_EXIT_FAIL);
}

JEMALLOC_ATTR(format(printf, 1, 2))
void
test_skip(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
	exit(JEMALLOC_TEST_EXIT_SKIP);
}
