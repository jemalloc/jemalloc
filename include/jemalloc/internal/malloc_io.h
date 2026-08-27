#ifndef JEMALLOC_INTERNAL_MALLOC_IO_H
#define JEMALLOC_INTERNAL_MALLOC_IO_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
/*
 * os/fmt.h directly (not the os.h umbrella): this header is itself included
 * from os/posix/error.h (for malloc_snprintf's declaration), so pulling in
 * the full os.h here would risk the same circular-include hazard os/error.h
 * avoids by not going through the umbrella either. os/fmt.h is a leaf module
 * (macros only, no dependency back on malloc_io.h or anything else), so
 * including it alone is safe.
 */
#include "jemalloc/internal/os/fmt.h"

/* Size of stack-allocated buffer passed to buferror(). */
#define BUFERROR_BUF 64

/*
 * Size of stack-allocated buffer used by malloc_{,v,vc}printf().  This must be
 * large enough for all possible uses within jemalloc.
 */
#define MALLOC_PRINTF_BUFSIZE 4096

write_cb_t wrtmessage;
#ifdef JEMALLOC_JET
write_cb_t *malloc_message_set(write_cb_t *write_cb);
#endif
int        buferror(int err, char *buf, size_t buflen);
uintmax_t  malloc_strtoumax(
     const char *restrict nptr, char **restrict endptr, int base);
void malloc_write(const char *s);

/*
 * malloc_vsnprintf() supports a subset of snprintf(3) that avoids floating
 * point math.
 */
size_t malloc_vsnprintf(char *str, size_t size, const char *format, va_list ap);
size_t malloc_snprintf(char *str, size_t size, const char *format, ...)
    JEMALLOC_FORMAT_PRINTF(3, 4);
/*
 * The caller can set write_cb to null to choose to print with the
 * je_malloc_message hook.
 */
void malloc_vcprintf(
    write_cb_t *write_cb, void *cbopaque, const char *format, va_list ap);
void malloc_cprintf(write_cb_t *write_cb, void *cbopaque, const char *format,
    ...) JEMALLOC_FORMAT_PRINTF(3, 4);
void malloc_printf(const char *format, ...) JEMALLOC_FORMAT_PRINTF(1, 2);

ssize_t malloc_write_fd(int fd, const void *buf, size_t count);
ssize_t malloc_read_fd(int fd, void *buf, size_t count);

static inline int
malloc_open(const char *path, int flags) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_open)
	return (int)syscall(SYS_open, path, flags);
#elif defined(JEMALLOC_USE_SYSCALL) && defined(SYS_openat)
	return (int)syscall(SYS_openat, AT_FDCWD, path, flags);
#else
	return open(path, flags);
#endif
}

static inline int
malloc_close(int fd) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_close)
	return (int)syscall(SYS_close, fd);
#else
	return close(fd);
#endif
}

#endif /* JEMALLOC_INTERNAL_MALLOC_IO_H */
