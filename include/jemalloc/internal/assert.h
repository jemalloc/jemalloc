#ifndef JEMALLOC_INTERNAL_ASSERT_H
#define JEMALLOC_INTERNAL_ASSERT_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/util.h"

/*
 * Define a custom assert() in order to reduce the chances of deadlock during
 * assertion failure.
 */
#ifndef assert
#define assert(e) do {							\
	if (unlikely(config_debug && !(e))) {				\
		malloc_printf(						\
		    "<jemalloc>: %s:%d: Failed assertion: \"%s\"\n",	\
		    __FILE__, __LINE__, #e);				\
		abort();						\
	}								\
} while (0)
#endif

#ifndef not_reached
#define not_reached() do {						\
	if (config_debug) {						\
		malloc_printf(						\
		    "<jemalloc>: %s:%d: Unreachable code reached\n",	\
		    __FILE__, __LINE__);				\
		abort();						\
	}								\
	unreachable();							\
} while (0)
#endif

#ifndef not_implemented
#define not_implemented() do {						\
	if (config_debug) {						\
		malloc_printf("<jemalloc>: %s:%d: Not implemented\n",	\
		    __FILE__, __LINE__);				\
		abort();						\
	}								\
} while (0)
#endif

#ifndef assert_not_implemented
#define assert_not_implemented(e) do {					\
	if (unlikely(config_debug && !(e))) {				\
		not_implemented();					\
	}								\
} while (0)
#endif

/* Use to assert a particular configuration, e.g., cassert(config_debug). */
#ifndef cassert
#define cassert(c) do {							\
	if (unlikely(!(c))) {						\
		not_reached();						\
	}								\
} while (0)
#endif

#if !defined(__cplusplus) && !defined(_MSC_VER) && !defined(static_assert)
#ifdef JEMALLOC_INTERNAL_HAS_STATIC_ASSERT
	#define static_assert _Static_assert
#else
	#define STATIC_ASSERT_TOKENPASTE_IMPL(a, b) a ## b
	#define STATIC_ASSERT_TOKENPASTE(a, b) STATIC_ASSERT_TOKENPASTE_IMPL(a, b)
	#define static_assert(expression, message) enum { STATIC_ASSERT_TOKENPASTE(static_assertion_, __LINE__) = 1 / ((message) && (expression)) }
#endif
#endif

#endif /* JEMALLOC_INTERNAL_ASSERT_H */
