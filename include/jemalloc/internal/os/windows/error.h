#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_ERROR_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_ERROR_H

/*
 * Windows error backend (GetLastError/SetLastError, FormatMessageA).
 */
#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE int
os_errno_get(void) {
	return GetLastError();
}

JEMALLOC_ALWAYS_INLINE void
os_errno_set(int errnum) {
	SetLastError(errnum);
}

JEMALLOC_ALWAYS_INLINE int
os_strerror(int err, char *buf, size_t buflen) {
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, (LPSTR)buf,
	    (DWORD)buflen, NULL);
	return 0;
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_ERROR_H */
