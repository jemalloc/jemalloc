#ifndef JEMALLOC_INTERNAL_OS_FREEBSD_OVERCOMMIT_H
#define JEMALLOC_INTERNAL_OS_FREEBSD_OVERCOMMIT_H

#include "jemalloc/internal/jemalloc_preamble.h"

#include <sys/mman.h>
#include <sys/sysctl.h>
#ifdef __FreeBSD__
#  include <vm/vm_param.h>
#endif

/* Defined in src/pages.c. */
extern int mmap_flags;

JEMALLOC_ALWAYS_INLINE bool
os_overcommits_sysctl(void) {
	int    vm_overcommit;
	size_t sz;

	sz = sizeof(vm_overcommit);
#  if defined(__FreeBSD__) && defined(VM_OVERCOMMIT)
	int mib[2];

	mib[0] = CTL_VM;
	mib[1] = VM_OVERCOMMIT;
	if (sysctl(mib, 2, &vm_overcommit, &sz, NULL, 0) != 0) {
		return false; /* Error. */
	}
#  else
	if (sysctlbyname("vm.overcommit", &vm_overcommit, &sz, NULL, 0) != 0) {
		return false; /* Error. */
	}
#  endif

	return ((vm_overcommit & 0x3) == 0);
}

JEMALLOC_ALWAYS_INLINE bool
os_overcommit_boot(void) {
	mmap_flags = MAP_PRIVATE | MAP_ANON;
	os_overcommits = os_overcommits_sysctl();
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_FREEBSD_OVERCOMMIT_H */
