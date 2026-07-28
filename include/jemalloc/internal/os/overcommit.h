#ifndef JEMALLOC_INTERNAL_OS_OVERCOMMIT_H
#define JEMALLOC_INTERNAL_OS_OVERCOMMIT_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/pages.h"

/*
 * Overcommit interface: boot-time detection of kernel memory-policy facts
 * that src/pages.c and os/vm.h's reserve/commit primitives depend on --
 * whether the kernel overcommits, and (Linux only) whether MADV_DONTNEED is
 * known to zero pages and what mmap() flags to use.  Separate from os/vm.h
 * because none of this is a VM reserve/commit primitive itself; it's boot-
 * time policy detection that some of those primitives happen to read.
 *
 * Unlike every other os/<module>.h, this one has FOUR tiers instead of the
 * usual two (posix/windows) or three (posix/windows/darwin): FreeBSD and
 * Linux each have substantial, disjoint, dedicated detection logic (real
 * sysctl/proc-file probing, not a one-line difference), so each gets its own
 * file with no internal #ifdef __FreeBSD__/__linux__ branching -- unlike
 * os/darwin/cpu.h (which duplicates two mostly-identical functions just to
 * override a third), splitting these out duplicates nothing, since
 * os_overcommits_sysctl() and os_overcommits_proc()/the DONTNEED-zeros probe
 * never had anything in common to begin with. Default: posix/ (NetBSD
 * hardcoded true, everyone else false -- matches this project's
 * pre-refactor behavior for all of them). Overrides: Windows (never
 * overcommits), FreeBSD/kFreeBSD (sysctl), Linux (/proc/sys/vm).
 *
 * State: `os_overcommits`, set by os_overcommit_boot(), read by os/vm.h's
 * backends. Defined in src/pages.c.
 */
extern bool os_overcommits;

JEMALLOC_ALWAYS_INLINE bool os_overcommit_boot(void);

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/overcommit.h"
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#  include "jemalloc/internal/os/freebsd/overcommit.h"
#elif defined(__linux__)
#  include "jemalloc/internal/os/linux/overcommit.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/overcommit.h"
#else
#  error "OS layer: no overcommit backend for this platform; add os/<os>/overcommit.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_OVERCOMMIT_H */
