#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa_hooks.h"

static void *hpa_hooks_map(size_t size);
static void hpa_hooks_unmap(void *ptr, size_t size);
static void hpa_hooks_purge(void *ptr, size_t size);
static void hpa_hooks_hugify(void *ptr, size_t size);
static void hpa_hooks_dehugify(void *ptr, size_t size);

hpa_hooks_t hpa_hooks_default = {
	&hpa_hooks_map,
	&hpa_hooks_unmap,
	&hpa_hooks_purge,
	&hpa_hooks_hugify,
	&hpa_hooks_dehugify,
};

static void *
hpa_hooks_map(size_t size) {
	bool commit = true;
	return pages_map(NULL, size, HUGEPAGE, &commit);
}

static void
hpa_hooks_unmap(void *ptr, size_t size) {
	pages_unmap(ptr, size);
}

static void
hpa_hooks_purge(void *ptr, size_t size) {
	pages_purge_forced(ptr, size);
}

static void
hpa_hooks_hugify(void *ptr, size_t size) {
	bool err = pages_huge(ptr, size);
	(void)err;
}

static void
hpa_hooks_dehugify(void *ptr, size_t size) {
	bool err = pages_nohuge(ptr, size);
	(void)err;
}
