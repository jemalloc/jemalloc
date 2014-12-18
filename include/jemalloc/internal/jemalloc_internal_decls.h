#ifndef JEMALLOC_INTERNAL_DECLS_H
#define	JEMALLOC_INTERNAL_DECLS_H

#include <math.h>
#ifdef _WIN32
#  include <windows.h>
#  define ENOENT ERROR_PATH_NOT_FOUND
#  define EINVAL ERROR_BAD_ARGUMENTS
#  define EAGAIN ERROR_OUTOFMEMORY
#  define EPERM  ERROR_WRITE_FAULT
#  define EFAULT ERROR_INVALID_ADDRESS
#  define ENOMEM ERROR_NOT_ENOUGH_MEMORY
#  undef ERANGE
#  define ERANGE ERROR_INVALID_DATA
#else
#  include <sys/param.h>
#  include <sys/mman.h>
#  if !defined(__pnacl__) && !defined(__native_client__)
#    include <sys/syscall.h>
#    if !defined(SYS_write) && defined(__NR_write)
#      define SYS_write __NR_write
#    endif
#    include <sys/uio.h>
#  endif
#  include <pthread.h>
#  include <errno.h>
#endif
#include <sys/types.h>

#include <limits.h>
#ifndef SIZE_T_MAX
#  define SIZE_T_MAX	SIZE_MAX
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#ifndef offsetof
#  define offsetof(type, member)	((size_t)&(((type *)NULL)->member))
#endif
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifdef _MSC_VER
#  include <io.h>
typedef intptr_t ssize_t;
#  define PATH_MAX 1024
#  define STDERR_FILENO 2
#  define __func__ __FUNCTION__
/* Disable warnings about deprecated system functions. */
#  pragma warning(disable: 4996)
#if _MSC_VER < 1800
static int
isblank(int c)
{
	return (c == '\t' || c == ' ');
}
#endif
#else
#  include <unistd.h>
#endif
#include <fcntl.h>

#endif /* JEMALLOC_INTERNAL_H */
