#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bit_util.h"
#include "jemalloc/internal/sc.h"

sc_data_t sc_data_global;

static size_t
reg_size_compute(int lg_base, int lg_delta, int ndelta) {
	return (ZU(1) << lg_base) + (ZU(ndelta) << lg_delta);
}

/* Returns the number of pages in the slab. */
static int
slab_size(int lg_page, int lg_base, int lg_delta, int ndelta) {
	size_t page = (ZU(1) << lg_page);
	size_t reg_size = reg_size_compute(lg_base, lg_delta, ndelta);

	size_t try_slab_size = page;
	size_t try_nregs = try_slab_size / reg_size;
	size_t perfect_slab_size = 0;
	bool perfect = false;
	/*
	 * This loop continues until we find the least common multiple of the
	 * page size and size class size.  Size classes are all of the form
	 * base + ndelta * delta == (ndelta + base/ndelta) * delta, which is
	 * (ndelta + ngroup) * delta.  The way we choose slabbing strategies
	 * means that delta is at most the page size and ndelta < ngroup.  So
	 * the loop executes for at most 2 * ngroup - 1 iterations, which is
	 * also the bound on the number of pages in a slab chosen by default.
	 * With the current default settings, this is at most 7.
	 */
	while (!perfect) {
		perfect_slab_size = try_slab_size;
		size_t perfect_nregs = try_nregs;
		try_slab_size += page;
		try_nregs = try_slab_size / reg_size;
		if (perfect_slab_size == perfect_nregs * reg_size) {
			perfect = true;
		}
	}
	return (int)(perfect_slab_size / page);
}

static void
size_class(
    /* Output. */
    sc_t *sc,
    /* Configuration decisions. */
    int lg_max_lookup, int lg_page, int lg_ngroup,
    /* Inputs specific to the size class. */
    int index, int lg_base, int lg_delta, int ndelta) {
	sc->index = index;
	sc->lg_base = lg_base;
	sc->lg_delta = lg_delta;
	sc->ndelta = ndelta;
	sc->psz = (reg_size_compute(lg_base, lg_delta, ndelta)
	    % (ZU(1) << lg_page) == 0);
	size_t size = (ZU(1) << lg_base) + (ZU(ndelta) << lg_delta);
	if (index == 0) {
		assert(!sc->psz);
	}
	if (size < (ZU(1) << (lg_page + lg_ngroup))) {
		sc->bin = true;
		sc->pgs = slab_size(lg_page, lg_base, lg_delta, ndelta);
	} else {
		sc->bin = false;
		sc->pgs = 0;
	}
	if (size <= (ZU(1) << lg_max_lookup)) {
		sc->lg_delta_lookup = lg_delta;
	} else {
		sc->lg_delta_lookup = 0;
	}
}

static void
size_classes(
    /* Output. */
    sc_data_t *sc_data,
    /* Determined by the system. */
    size_t lg_ptr_size, int lg_quantum,
    /* Configuration decisions. */
    int lg_tiny_min, int lg_max_lookup, int lg_page, int lg_ngroup) {
	int ptr_bits = (1 << lg_ptr_size) * 8;
	int ngroup = (1 << lg_ngroup);
	int ntiny = 0;
	int nlbins = 0;
	int lg_tiny_maxclass = (unsigned)-1;
	int nbins = 0;
	int npsizes = 0;

	int index = 0;

	int ndelta = 0;
	int lg_base = lg_tiny_min;
	int lg_delta = lg_base;

	/* Outputs that we update as we go. */
	size_t lookup_maxclass = 0;
	size_t small_maxclass = 0;
	int lg_large_minclass = 0;
	size_t large_maxclass = 0;

	/* Tiny size classes. */
	while (lg_base < lg_quantum) {
		sc_t *sc = &sc_data->sc[index];
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		if (sc->lg_delta_lookup != 0) {
			nlbins = index + 1;
		}
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
		ntiny++;
		/* Final written value is correct. */
		lg_tiny_maxclass = lg_base;
		index++;
		lg_delta = lg_base;
		lg_base++;
	}

	/* First non-tiny (pseudo) group. */
	if (ntiny != 0) {
		sc_t *sc = &sc_data->sc[index];
		/*
		 * See the note in sc.h; the first non-tiny size class has an
		 * unusual encoding.
		 */
		lg_base--;
		ndelta = 1;
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		index++;
		lg_base++;
		lg_delta++;
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
	}
	while (ndelta < ngroup) {
		sc_t *sc = &sc_data->sc[index];
		size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
		    lg_base, lg_delta, ndelta);
		index++;
		ndelta++;
		if (sc->psz) {
			npsizes++;
		}
		if (sc->bin) {
			nbins++;
		}
	}

	/* All remaining groups. */
	lg_base = lg_base + lg_ngroup;
	while (lg_base < ptr_bits - 1) {
		ndelta = 1;
		int ndelta_limit;
		if (lg_base == ptr_bits - 2) {
			ndelta_limit = ngroup - 1;
		} else {
			ndelta_limit = ngroup;
		}
		while (ndelta <= ndelta_limit) {
			sc_t *sc = &sc_data->sc[index];
			size_class(sc, lg_max_lookup, lg_page, lg_ngroup, index,
			    lg_base, lg_delta, ndelta);
			if (sc->lg_delta_lookup != 0) {
				nlbins = index + 1;
				/* Final written value is correct. */
				lookup_maxclass = (ZU(1) << lg_base)
				    + (ZU(ndelta) << lg_delta);
			}
			if (sc->psz) {
				npsizes++;
			}
			if (sc->bin) {
				nbins++;
				/* Final written value is correct. */
				small_maxclass = (ZU(1) << lg_base)
				    + (ZU(ndelta) << lg_delta);
				if (lg_ngroup > 0) {
					lg_large_minclass = lg_base + 1;
				} else {
					lg_large_minclass = lg_base + 2;
				}
			}
			large_maxclass = (ZU(1) << lg_base)
			    + (ZU(ndelta) << lg_delta);
			index++;
			ndelta++;
		}
		lg_base++;
		lg_delta++;
	}
	/* Additional outputs. */
	int nsizes = index;
	unsigned lg_ceil_nsizes = lg_ceil(nsizes);

	/* Fill in the output data. */
	sc_data->ntiny = ntiny;
	sc_data->nlbins = nlbins;
	sc_data->nbins = nbins;
	sc_data->nsizes = nsizes;
	sc_data->lg_ceil_nsizes = lg_ceil_nsizes;
	sc_data->npsizes = npsizes;
	sc_data->lg_tiny_maxclass = lg_tiny_maxclass;
	sc_data->lookup_maxclass = lookup_maxclass;
	sc_data->small_maxclass = small_maxclass;
	sc_data->lg_large_minclass = lg_large_minclass;
	sc_data->large_minclass = (ZU(1) << lg_large_minclass);
	sc_data->large_maxclass = large_maxclass;
}

/*
 * Defined later (after size_classes.h becomes visible), but called during
 * initialization.
 */
static void sc_data_assert(sc_data_t *sc_data);

void
sc_data_init(sc_data_t *sc_data) {
	assert(!sc_data->initialized);

	int lg_max_lookup = 12;

	size_classes(sc_data, LG_SIZEOF_PTR, LG_QUANTUM, SC_LG_TINY_MIN,
	    lg_max_lookup, LG_PAGE, 2);

	sc_data->initialized = true;

	sc_data_assert(sc_data);
}

void
sc_boot() {
	sc_data_init(&sc_data_global);
}

/*
 * We don't include size_classes.h until this point, to ensure only the asserts
 * can see it.
 */
#include "jemalloc/internal/size_classes.h"

static void
sc_assert(sc_t *sc, int index, int lg_base, int lg_delta, int ndelta, int psz,
    int bin, int pgs, int lg_delta_lookup) {
	assert(sc->index == index);
	assert(sc->lg_base == lg_base);
	assert(sc->lg_delta == lg_delta);
	assert(sc->ndelta == ndelta);
	assert(sc->psz == psz);
	assert(sc->bin == bin);
	assert(sc->pgs == pgs);
	assert(sc->lg_delta_lookup == lg_delta_lookup);
}

static void
sc_data_assert(sc_data_t *sc_data) {
	assert(SC_NTINY == NTBINS);
	assert(SC_NSIZES == NSIZES);
	assert(SC_NBINS == NBINS);
	assert(NPSIZES <= SC_NPSIZES_MAX);
	assert(sc_data->ntiny == NTBINS);
	assert(sc_data->nlbins == NLBINS);
	assert(sc_data->nbins == NBINS);
	assert(sc_data->nsizes == NSIZES);
	assert(sc_data->lg_ceil_nsizes == LG_CEIL_NSIZES);
	assert(sc_data->npsizes == NPSIZES);
#if NTBINS > 0
	assert(sc_data->lg_tiny_maxclass == LG_TINY_MAXCLASS);
#else
	assert(sc_data->lg_tiny_maxclass == -1);
#endif
	assert(sc_data->lookup_maxclass == LOOKUP_MAXCLASS);
	assert(sc_data->small_maxclass == SMALL_MAXCLASS);
	assert(sc_data->lg_large_minclass == LG_LARGE_MINCLASS);
	assert(sc_data->large_minclass == LARGE_MINCLASS);
	assert(sc_data->large_maxclass == LARGE_MAXCLASS);
	assert(sc_data->initialized);
#define no 0
#define yes 1
#define SC(index, lg_base, lg_delta, ndelta, psz, bin, pgs,		\
    lg_delta_lookup)							\
	sc_assert(&sc_data->sc[index], index, lg_base, lg_delta,	\
	    ndelta, psz, bin, pgs, lg_delta_lookup);
	SIZE_CLASSES
#undef no
#undef yes
#undef SC
}
