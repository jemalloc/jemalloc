#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

ph_gen(, extent_avail_, extent_tree_t, extent_t, ph_link,
    extent_esnead_comp)
ph_gen(, extent_heap_, extent_heap_t, extent_t, ph_link, extent_snad_comp)
