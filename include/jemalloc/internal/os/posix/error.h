#ifndef JEMALLOC_INTERNAL_OS_POSIX_ERROR_H
#define JEMALLOC_INTERNAL_OS_POSIX_ERROR_H

/*
 * POSIX error backend (errno, strerror_r).
 */
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

JEMALLOC_ALWAYS_INLINE int
os_errno_get(void) {
	return errno;
}

JEMALLOC_ALWAYS_INLINE void
os_errno_set(int errnum) {
	errno = errnum;
}

/*
 * glibc provides a non-standard strerror_r() when _GNU_SOURCE is defined, so
 * provide a wrapper.
 */
JEMALLOC_ALWAYS_INLINE int
os_strerror(int err, char *buf, size_t buflen) {
#if defined(JEMALLOC_STRERROR_R_RETURNS_CHAR_WITH_GNU_SOURCE)                  \
    && defined(_GNU_SOURCE)
	char *b = strerror_r(err, buf, buflen);
	if (b != buf) {
		malloc_snprintf(buf, buflen, "%s", b);
	}
	return 0;
#else
	return strerror_r(err, buf, buflen);
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_ERROR_H */
