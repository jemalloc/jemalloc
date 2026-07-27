#ifndef JEMALLOC_INTERNAL_OS_POSIX_FILE_H
#define JEMALLOC_INTERNAL_OS_POSIX_FILE_H

/*
 * POSIX file-I/O backend (read/write syscalls). Direct syscalls where
 * available.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

#ifdef JEMALLOC_USE_SYSCALL
#  include <sys/syscall.h>
#endif

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write_once(int fd, const void *buf, size_t bytes) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_write)
	return (ssize_t)syscall(SYS_write, fd, buf, bytes);
#else
	return (ssize_t)write(fd, buf, bytes);
#endif
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_read_once(int fd, void *buf, size_t bytes) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_read)
	return (ssize_t)syscall(SYS_read, fd, buf, bytes);
#else
	return (ssize_t)read(fd, buf, bytes);
#endif
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write(int fd, const void *buf, size_t bytes) {
	size_t bytes_written = 0;
	do {
		ssize_t result = os_file_write_once(fd,
		    &((const byte_t *)buf)[bytes_written], bytes - bytes_written);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
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
			if (errno == EINTR) {
				continue;
			}
			return result;
		} else if (result == 0) {
			break;
		}
		bytes_read += result;
	} while (bytes_read < bytes);
	return bytes_read;
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_FILE_H */
