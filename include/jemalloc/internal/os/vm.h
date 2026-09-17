#ifndef JEMALLOC_INTERNAL_OS_VM_H
#define JEMALLOC_INTERNAL_OS_VM_H

#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/os/detect.h"
#include "jemalloc/internal/os/overcommit.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

/*
 * VM interface: raw page-level reserve/commit/decommit/guard/trim primitives
 * used by src/pages.c. Platform-specific fast paths (e.g. the FreeBSD
 * MAP_EXCL reservation shortcut) stay in src/pages.c, since they are
 * portable decisions layered on top of these primitives, not primitives
 * themselves.
 * Default: posix/.  Override: Windows (VirtualAlloc/VirtualFree).
 *
 * `os_overcommits` (read by os_vm_reserve/os_vm_commit_impl below) is
 * declared and set by os/overcommit.h, a separate module -- boot-time
 * kernel-policy detection isn't itself a VM reserve/commit primitive.
 */

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE void *os_vm_reserve(
    void *hint, size_t size, size_t alignment, bool *commit);
JEMALLOC_ALWAYS_INLINE void os_vm_release(void *addr, size_t size);
JEMALLOC_ALWAYS_INLINE void *os_vm_trim(void *addr, size_t alloc_size,
    size_t leadsize, size_t size, bool *commit);
JEMALLOC_ALWAYS_INLINE bool os_vm_commit(void *addr, size_t size);
JEMALLOC_ALWAYS_INLINE bool os_vm_decommit(void *addr, size_t size);
JEMALLOC_ALWAYS_INLINE void os_vm_mark_guards(void *head, void *tail);
JEMALLOC_ALWAYS_INLINE void os_vm_unmark_guards(void *head, void *tail);
JEMALLOC_ALWAYS_INLINE bool os_vm_purge_lazy(void *addr, size_t size);
JEMALLOC_ALWAYS_INLINE size_t os_vm_page_size(void);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/vm.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/vm.h"
#else
#  error "OS layer: no vm backend for this platform; add os/<os>/vm.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_VM_H */
