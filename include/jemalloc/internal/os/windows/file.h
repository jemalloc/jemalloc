#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_FILE_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_FILE_H

/*
 * Windows file-I/O backend, via the C runtime io.h (_read/_write).
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#include <io.h>

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write_once(int fd, const void *buf, size_t bytes) {
	return (ssize_t)write(fd, buf, (unsigned int)bytes);
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_read_once(int fd, void *buf, size_t bytes) {
	return (ssize_t)read(fd, buf, (unsigned int)bytes);
}

/* No EINTR/signal-interruption semantics on Windows: never retry on error. */
JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write(int fd, const void *buf, size_t bytes) {
	size_t bytes_written = 0;
	do {
		ssize_t result = os_file_write_once(fd,
		    &((const byte_t *)buf)[bytes_written], bytes - bytes_written);
		if (result < 0) {
			return result;
		}
		bytes_written += result;
	} while (bytes_written < bytes);
	return bytes_written;
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_read(int fd, void *buf, size_t bytes) {
	size_t bytes_read = 0;
	do {
		ssize_t result = os_file_read_once(
		    fd, &((byte_t *)buf)[bytes_read], bytes - bytes_read);
		if (result < 0) {
			return result;
		} else if (result == 0) {
			break;
		}
		bytes_read += result;
	} while (bytes_read < bytes);
	return bytes_read;
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_FILE_H */
