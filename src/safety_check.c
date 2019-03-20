#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

void safety_check_fail(const char *format, ...) {
	va_list ap;

	va_start(ap, format);
	malloc_vcprintf(NULL, NULL, format, ap);
	va_end(ap);
	abort();
}
