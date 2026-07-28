#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_VM_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_VM_H

#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE void *
os_vm_reserve(void *hint, size_t size, size_t alignment, bool *commit) {
	assert(ALIGNMENT_ADDR2BASE(hint, os_page) == hint);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	assert(size != 0);
	(void)alignment;

	if (os_overcommits) {
		*commit = true;
	}

	void *ret;
	/*
	 * If VirtualAlloc can't allocate at the given address when one is
	 * given, it fails and returns NULL.
	 */
	ret = VirtualAlloc(hint, size, MEM_RESERVE | (*commit ? MEM_COMMIT : 0),
	    PAGE_READWRITE);
	assert(ret == NULL || (hint == NULL && ret != hint)
	    || (hint != NULL && ret == hint));
	return ret;
}

JEMALLOC_ALWAYS_INLINE void
os_vm_release(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);

	if (VirtualFree(addr, 0, MEM_RELEASE) == 0) {
		char buf[BUFERROR_BUF];

		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in VirtualFree(): %s\n", buf);
		if (opt_abort) {
			abort();
		}
	}
}

JEMALLOC_ALWAYS_INLINE void *
os_vm_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size,
    bool *commit) {
	assert(alloc_size >= leadsize + size);
	void *ret = (void *)((byte_t *)addr + leadsize);

	os_vm_release(addr, alloc_size);
	void *new_addr = os_vm_reserve(ret, size, PAGE, commit);
	if (new_addr == ret) {
		return ret;
	}
	if (new_addr != NULL) {
		os_vm_release(new_addr, size);
	}
	return NULL;
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_commit_impl(void *addr, size_t size, bool commit) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	return (commit
	        ? (addr != VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE))
	        : (!VirtualFree(addr, size, MEM_DECOMMIT)));
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_commit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, true);
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_decommit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, false);
}

JEMALLOC_ALWAYS_INLINE void
os_vm_mark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
	/* Decommit sets the region to MEM_DECOMMIT. */
	if (head != NULL) {
		os_vm_decommit(head, PAGE);
	}
	if (tail != NULL) {
		os_vm_decommit(tail, PAGE);
	}
}

JEMALLOC_ALWAYS_INLINE void
os_vm_unmark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
	if (head != NULL) {
		os_vm_commit(head, PAGE);
	}
	if (tail != NULL) {
		os_vm_commit(tail, PAGE);
	}
}

JEMALLOC_ALWAYS_INLINE bool
os_vm_purge_lazy(void *addr, size_t size) {
	VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_VM_H */
