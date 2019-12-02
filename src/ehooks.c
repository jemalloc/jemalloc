#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ehooks.h"

void ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks) {
	ehooks_set_extent_hooks_ptr(ehooks, extent_hooks);
}
