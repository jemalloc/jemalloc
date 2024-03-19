#ifndef JEMALLOC_INTERNAL_PID_NAMESPACE_EXTERNS_H
#define JEMALLOC_INTERNAL_PID_NAMESPACE_EXTERNS_H


#include "jemalloc/internal/jemalloc_preamble.h"

#ifdef JEMALLOC_PID_NAMESPACE
#include <stddef.h>
#include <sys/types.h>

ssize_t pid_namespace();
#endif /* JEMALLOC_PID_NAMESPACE */

#endif /* JEMALLOC_INTERNAL_PID_NAMESPACE_EXTERNS_H */
