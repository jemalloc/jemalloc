#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/size_classes.h"

sc_data_t sc_data_global;

static void
fill_sc(sc_data_t *data, int index, int lg_base, int lg_delta, int ndelta,
    bool psz, bool bin, int pgs, int lg_delta_lookup) {
	sc_t *sc = &data->sc[index];
	sc->index = index;
	sc->lg_base = lg_base;
	sc->lg_delta = lg_delta;
	sc->ndelta = ndelta;
	sc->psz = psz;
	sc->bin = bin;
	sc->pgs = pgs;
	sc->lg_delta_lookup = lg_delta_lookup;
}

void
sc_data_init(sc_data_t *data) {
	assert(SC_NTINY == NTBINS);
	assert(SC_NSIZES == NSIZES);
	assert(SC_NBINS == NBINS);
	assert(NPSIZES <= SC_NPSIZES_MAX);
	assert(!data->initialized);
	data->initialized = true;
	data->ntiny = NTBINS;
	data->nlbins = NLBINS;
	data->nbins = NBINS;
	data->nsizes = NSIZES;
	data->lg_ceil_nsizes = LG_CEIL_NSIZES;
	data->npsizes = NPSIZES;
#if SC_NTINY != 0
	data->lg_tiny_maxclass = LG_TINY_MAXCLASS;
#else
	data->lg_tiny_maxclass = -1;
#endif
	data->lookup_maxclass = LOOKUP_MAXCLASS;
	data->small_maxclass = SMALL_MAXCLASS;
	data->lg_large_minclass = LG_LARGE_MINCLASS;
	data->large_minclass = LARGE_MINCLASS;
	data->large_maxclass = LARGE_MAXCLASS;
#define no 0
#define yes 1
#define SC(index, lg_base_base, lg_delta, ndelta, psz, bin, pgs,	\
    lg_delta_lookup)							\
	fill_sc(data, index, lg_base_base, lg_delta, ndelta, psz, bin, 	\
	    pgs, lg_delta_lookup);
	SIZE_CLASSES
#undef no
#undef yes
#undef SC
}

void
sc_boot() {
	sc_data_init(&sc_data_global);
}
