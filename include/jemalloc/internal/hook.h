#ifndef JEMALLOC_INTERNAL_HOOK_H
#define JEMALLOC_INTERNAL_HOOK_H

#include "jemalloc/internal/tsd.h"

/*
 * This API is *extremely* experimental, and may get ripped out, changed in API-
 * and ABI-incompatible ways, be insufficiently or incorrectly documented, etc.
 *
 * It allows hooking the stateful parts of the API to see changes as they
 * happen.
 *
 * Allocation hooks are called after the allocation is done, free hooks are
 * called before the free is done, and expand hooks are called after the
 * allocation is expanded.
 *
 * For realloc and rallocx, if the expansion happens in place, the expansion
 * hook is called.  If it is moved, then the alloc hook is called on the new
 * location, and then the free hook is called on the old location.
 *
 * (We omit no-ops, like free(NULL), etc.).
 *
 * Reentrancy:
 *   Is not protected against.  If your hooks allocate, then the hooks will be
 *   called again.  Note that you can guard against this with a thread-local
 *   "in_hook" bool.
 * Threading:
 *   The installation of a hook synchronizes with all its uses.  If you can
 *   prove the installation of a hook happens-before a jemalloc entry point,
 *   then the hook will get invoked (unless there's a racing removal).
 *
 *   Hook insertion appears to be atomic at a per-thread level (i.e. if a thread
 *   allocates and has the alloc hook invoked, then a subsequent free on the
 *   same thread will also have the free hook invoked).
 *
 *   The *removal* of a hook does *not* block until all threads are done with
 *   the hook.  Hook authors have to be resilient to this, and need some
 *   out-of-band mechanism for cleaning up any dynamically allocated memory
 *   associated with their hook.
 * Ordering:
 *   Order of hook execution is unspecified, and may be different than insertion
 *   order.
 */

enum hook_alloc_e {
	hook_alloc_malloc,
	hook_alloc_posix_memalign,
	hook_alloc_aligned_alloc,
	hook_alloc_calloc,
	hook_alloc_memalign,
	hook_alloc_valloc,
	hook_alloc_mallocx,

	/* The reallocating functions have both alloc and dalloc variants */
	hook_alloc_realloc,
	hook_alloc_rallocx,
};
/*
 * We put the enum typedef after the enum, since this file may get included by
 * jemalloc_cpp.cpp, and C++ disallows enum forward declarations.
 */
typedef enum hook_alloc_e hook_alloc_t;

enum hook_dalloc_e {
	hook_dalloc_free,
	hook_dalloc_dallocx,
	hook_dalloc_sdallocx,

	/*
	 * The dalloc halves of reallocation (not called if in-place expansion
	 * happens).
	 */
	hook_dalloc_realloc,
	hook_dalloc_rallocx,
};
typedef enum hook_dalloc_e hook_dalloc_t;


enum hook_expand_e {
	hook_expand_realloc,
	hook_expand_rallocx,
	hook_expand_xallocx,
};
typedef enum hook_expand_e hook_expand_t;

typedef void (*hook_alloc)(
    void *extra, hook_alloc_t type, void *result, uintptr_t result_raw,
    uintptr_t args_raw[3]);

typedef void (*hook_dalloc)(
    void *extra, hook_dalloc_t type, void *address, uintptr_t args_raw[3]);

typedef void (*hook_expand)(
    void *extra, hook_expand_t type, void *address, size_t old_usize,
    size_t new_usize, uintptr_t result_raw, uintptr_t args_raw[4]);

typedef struct hooks_s hooks_t;
struct hooks_s {
	hook_alloc alloc_hook;
	hook_dalloc dalloc_hook;
	hook_expand expand_hook;
};

/*
 * Returns an opaque handle to be used when removing the hook.  NULL means that
 * we couldn't install the hook.
 */
bool hook_boot();

void *hook_install(tsdn_t *tsdn, hooks_t *hooks, void *extra);
/* Uninstalls the hook with the handle previously returned from hook_install. */
void hook_remove(tsdn_t *tsdn, void *opaque);

/* Hooks */

void hook_invoke_alloc(hook_alloc_t type, void *result, uintptr_t result_raw,
    uintptr_t args_raw[3]);

void hook_invoke_dalloc(hook_dalloc_t type, void *address,
    uintptr_t args_raw[3]);

void hook_invoke_expand(hook_expand_t type, void *address, size_t old_usize,
    size_t new_usize, uintptr_t result_raw, uintptr_t args_raw[4]);

#endif /* JEMALLOC_INTERNAL_HOOK_H */
