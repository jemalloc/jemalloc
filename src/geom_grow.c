#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

void
geom_grow_init(geom_grow_t *geom_grow) {
	geom_grow->next = sz_psz2ind(HUGEPAGE);
	geom_grow->limit = sz_psz2ind(SC_LARGE_MAXCLASS);
}
