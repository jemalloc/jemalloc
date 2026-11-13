#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/deferral.h"
#include "jemalloc/internal/hpa_shard_set.h"

const size_t hpa_shard_set_band_max[HPA_SHARD_SET_NBANDS] = {4 * 1024, 8 * 1024,
    16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024,
    HUGEPAGE};

hpa_shard_sets_t hpa_shard_sets_global;

/*
 * The largest possible usable size the HPA serves, inclusive.  Small sizes
 * up to this maximum can still go to the PAC if slab_sizes makes their slab
 * larger than the HPA supports.
 */
static size_t
hpa_shard_sets_usize_max(size_t slab_max_alloc) {
	if (slab_max_alloc < SC_SMALL_MAXCLASS + sz_large_pad) {
		return SC_SMALL_MAXCLASS;
	}
	assert(slab_max_alloc > sz_large_pad);
	return slab_max_alloc - sz_large_pad;
}

/*
 * Returns true if the configuration cannot be used, without printing; the
 * caller decides how to report it.
 */
bool
hpa_shard_sets_config_validate(
    const hpa_shard_sets_config_t *config, size_t slab_max_alloc) {
	/* The first band has no band below it to join. */
	if (config->nshards[0] == 0) {
		return true;
	}
	for (unsigned i = 0; i < HPA_SHARD_SET_NBANDS; i++) {
		/* A set cannot have more than HPA_MAX_SHARDS_PER_SET shards. */
		if (config->nshards[i] > HPA_MAX_SHARDS_PER_SET) {
			return true;
		}
	}
	/*
	 * Sizes the HPA serves above the last band would have no shard set,
	 * and pa_alloc() would silently fall through to the PAC.
	 */
	if (hpa_shard_sets_usize_max(slab_max_alloc)
	    > hpa_shard_set_band_max[HPA_SHARD_SET_NBANDS - 1]) {
		return true;
	}
	return false;
}

/*
 * Returns the first set whose allocation-size bound covers usize, or NULL if
 * usize is above the last bound.
 */
hpa_shard_set_t *
hpa_shard_sets_lookup(hpa_shard_sets_t *sets, size_t usize) {
	assert(sets->initialized);
	assert(usize > 0);
	for (unsigned i = 0; i < sets->nsets; i++) {
		if (usize <= sets->sets[i].size_max) {
			return &sets->sets[i];
		}
	}
	return NULL;
}

/*
 * Initializes shard_set with nshards shards, none of them created yet.
 * Returns true on error.
 */
static bool
hpa_shard_set_init(tsdn_t *tsdn, hpa_shard_set_t *shard_set,
    hpa_shard_sets_t *sets, unsigned nshards, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts) {
	shard_set->sets = sets;
	shard_set->nshards = nshards;
	shard_set->opts = *opts;
	shard_set->sec_opts = *sec_opts;
	shard_set->shards = (atomic_p_t *)base_alloc(
	    tsdn, sets->base, nshards * sizeof(atomic_p_t), CACHELINE);
	if (shard_set->shards == NULL) {
		return true;
	}
	for (unsigned i = 0; i < nshards; i++) {
		atomic_store_p(&shard_set->shards[i], NULL, ATOMIC_RELAXED);
	}
	return malloc_mutex_init(&shard_set->init_mtx, "hpa_shard_set_init",
	    WITNESS_RANK_HPA_SHARD_SET, malloc_mutex_rank_exclusive);
}

bool
hpa_shard_sets_boot(tsdn_t *tsdn, hpa_shard_sets_t *sets, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_shard_sets_config_t *config, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts) {
	assert(!sets->initialized);
	assert(hpa_supported());

	if (hpa_shard_sets_config_validate(config, opts->slab_max_alloc)) {
		return true;
	}

	/*
	 * nsets stays 0 until every set is built, so a failed boot leaves no
	 * half-built set for the fork hooks or lookup to touch.  Shard pointer
	 * arrays already taken from base are reclaimed only with the base.
	 */
	memset(sets, 0, sizeof(*sets));
	sets->base = base;
	sets->central = central;
	sets->emap = emap;
	sets->edata_cache = edata_cache;

	/*
	 * Bands that start above the largest usable size the HPA serves need no
	 * set, and the last set ends at that size.
	 */
	size_t   usize_max = hpa_shard_sets_usize_max(opts->slab_max_alloc);
	unsigned nsets = 0;
	size_t   band_min = 1;
	for (unsigned i = 0; i < HPA_SHARD_SET_NBANDS && band_min <= usize_max;
	    i++) {
		size_t band_max = hpa_shard_set_band_max[i];
		assert(band_max >= band_min);
		/* A band without shards joins the set of the band below. */
		if (config->nshards[i] != 0) {
			if (hpa_shard_set_init(tsdn, &sets->sets[nsets], sets,
			        config->nshards[i], opts, sec_opts)) {
				return true;
			}
			nsets++;
		}
		assert(nsets > 0);
		sets->sets[nsets - 1].size_max = band_max < usize_max
		    ? band_max
		    : usize_max;
		band_min = band_max + 1;
	}

	sets->nsets = nsets;
	sets->initialized = true;
	return false;
}

hpa_shard_t *
hpa_shard_choose_hard(
    tsdn_t *tsdn, hpa_shard_set_t *shard_set, unsigned affinity) {
	assert(shard_set->nshards > 0);

	unsigned idx = affinity % shard_set->nshards;
	malloc_mutex_lock(tsdn, &shard_set->init_mtx);
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &shard_set->shards[idx], ATOMIC_RELAXED);
	if (shard == NULL) {
		hpa_shard_sets_t *sets = shard_set->sets;
		shard = (hpa_shard_t *)base_alloc(tsdn, sets->base,
		    sizeof(hpa_shard_t), CACHELINE);
		if (shard == NULL || hpa_shard_init(tsdn, shard, sets->central,
		        sets->emap, sets->base, sets->edata_cache, idx,
		        &shard_set->opts, &shard_set->sec_opts)) {
			shard = NULL;
		} else {
			atomic_store_p(&shard_set->shards[idx], shard,
			    ATOMIC_RELEASE);
		}
	}
	malloc_mutex_unlock(tsdn, &shard_set->init_mtx);
	return shard;
}

typedef void (*hpa_shard_sets_shard_cb_t)(tsdn_t *, hpa_shard_t *);

/* Calls cb on every initialized shard of every set. */
static void
hpa_shard_sets_for_each_shard(
    tsdn_t *tsdn, hpa_shard_sets_t *sets, hpa_shard_sets_shard_cb_t cb) {
	for (unsigned i = 0; i < sets->nsets; i++) {
		hpa_shard_set_t *shard_set = &sets->sets[i];
		for (unsigned j = 0; j < shard_set->nshards; j++) {
			hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
			    &shard_set->shards[j], ATOMIC_ACQUIRE);
			if (shard != NULL) {
				cb(tsdn, shard);
			}
		}
	}
}

void
hpa_shard_sets_prefork2(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	for (unsigned i = 0; i < sets->nsets; i++) {
		malloc_mutex_prefork(tsdn, &sets->sets[i].init_mtx);
	}
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_prefork2);
}

void
hpa_shard_sets_prefork3(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_prefork3);
}

void
hpa_shard_sets_prefork4(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_prefork4);
}

void
hpa_shard_sets_postfork_parent(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_postfork_parent);
	for (unsigned i = sets->nsets; i-- > 0;) {
		malloc_mutex_postfork_parent(tsdn, &sets->sets[i].init_mtx);
	}
}

void
hpa_shard_sets_postfork_child(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_postfork_child);
	for (unsigned i = sets->nsets; i-- > 0;) {
		malloc_mutex_postfork_child(tsdn, &sets->sets[i].init_mtx);
		/* The child has no background threads to defer work to. */
		sets->sets[i].opts.deferral_allowed = false;
	}
}

void
hpa_shard_sets_set_deferral_allowed(
    tsdn_t *tsdn, hpa_shard_sets_t *sets, bool deferral_allowed) {
	for (unsigned i = 0; i < sets->nsets; i++) {
		hpa_shard_set_t *shard_set = &sets->sets[i];
		/* Under init_mtx, so that no shard is created with the old one. */
		malloc_mutex_lock(tsdn, &shard_set->init_mtx);
		shard_set->opts.deferral_allowed = deferral_allowed;
		for (unsigned j = 0; j < shard_set->nshards; j++) {
			hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
			    &shard_set->shards[j], ATOMIC_ACQUIRE);
			if (shard != NULL) {
				hpa_shard_set_deferral_allowed(
				    tsdn, shard, deferral_allowed);
			}
		}
		malloc_mutex_unlock(tsdn, &shard_set->init_mtx);
	}
}

void
hpa_shard_sets_do_deferred_work(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_do_deferred_work);
}

uint64_t
hpa_shard_sets_time_until_deferred_work(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	uint64_t time = DEFERRED_WORK_MAX;
	for (unsigned i = 0; i < sets->nsets; i++) {
		hpa_shard_set_t *shard_set = &sets->sets[i];
		for (unsigned j = 0; j < shard_set->nshards; j++) {
			hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
			    &shard_set->shards[j], ATOMIC_ACQUIRE);
			if (shard == NULL) {
				continue;
			}
			uint64_t shard_time = hpa_time_until_deferred_work(
			    tsdn, shard);
			if (shard_time == DEFERRED_WORK_MIN) {
				return shard_time;
			}
			if (shard_time < time) {
				time = shard_time;
			}
		}
	}
	return time;
}

void
hpa_shard_sets_flush(tsdn_t *tsdn, hpa_shard_sets_t *sets) {
	hpa_shard_sets_for_each_shard(tsdn, sets, hpa_shard_flush);
}
