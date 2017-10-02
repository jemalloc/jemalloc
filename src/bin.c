#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/bin.h"

const bin_info_t bin_infos[NBINS] = {
#define BIN_INFO_bin_yes(reg_size, slab_size, nregs)			\
	{reg_size, slab_size, nregs, BITMAP_INFO_INITIALIZER(nregs)},
#define BIN_INFO_bin_no(reg_size, slab_size, nregs)
#define SC(index, lg_grp, lg_delta, ndelta, psz, bin, pgs,		\
    lg_delta_lookup)							\
	BIN_INFO_bin_##bin((1U<<lg_grp) + (ndelta<<lg_delta),		\
	    (pgs << LG_PAGE), (pgs << LG_PAGE) / ((1U<<lg_grp) +	\
	    (ndelta<<lg_delta)))
	SIZE_CLASSES
#undef BIN_INFO_bin_yes
#undef BIN_INFO_bin_no
#undef SC
};


