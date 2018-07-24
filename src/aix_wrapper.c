#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#ifdef JEMALLOC_AIX_WRAPPERS

#include <malloc.h>

static bool initialized = false;

void *__malloc__(size_t size)
{
	return initialized ? je_malloc(size) : bootstrap_malloc(size);
}

void __free__(void *ptr)
{
	if (initialized)
		je_free(ptr);
	else
		bootstrap_free(ptr);
}

void *__realloc__(void *ptr, size_t size)
{
	return je_realloc(ptr, size);
}

void *__calloc__(size_t numb, size_t size)
{
	return initialized ? je_calloc(numb, size) : bootstrap_calloc(numb, size);
}

int __posix_memalign__(void **memptr, size_t alignment, size_t size)
{
	return je_posix_memalign(memptr, alignment, size);
}

int __mallopt__(int command, int value)
{
	/* FIXME: implement at least M_MALIGN */
	return 0;
}

struct mallinfo __mallinfo__()
{
	/* FIXME: check if some fields can be filled */
	struct mallinfo mi;
	memset(&mi, 0, sizeof(mi));
	return mi;
}

void __malloc_init__(void)
{
}

void __malloc_prefork_lock__(void)
{
}

void __malloc_postfork_unlock__(void)
{
}

#endif
