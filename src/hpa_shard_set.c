#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/hpa_shard_set.h"

const char *const hpa_shard_set_pick_type_names[] = {"arena", "roundrobin"};

/*
 * Returns true if the configuration cannot be used, without printing; the
 * caller decides how to report it.  A configuration that does not describe a
 * function covering [1, slab_max_alloc] cannot be used, because the router
 * would then have sizes with no shard set and pa_alloc() would silently fall
 * through to the PAC.
 */
bool
hpa_shard_sets_config_validate(
    const hpa_shard_sets_config_t *config, size_t slab_max_alloc) {
	if (config->nsets == 0 || config->nsets > HPA_MAX_SHARD_SETS) {
		return true;
	}

	size_t prev_max = 0;
	for (unsigned i = 0; i < config->nsets; i++) {
		size_t size_max = config->sets[i].size_max;
		if (config->sets[i].nshards == 0) {
			return true;
		}
		if (config->sets[i].pick >= hpa_shard_set_pick_limit) {
			return true;
		}
		/* A set cannot pick more shards than it has slots. */
		if (config->sets[i].nshards > HPA_MAX_SHARDS_PER_SET) {
			return true;
		}
		if (size_max <= prev_max) {
			return true;
		}
		if (size_max > HUGEPAGE) {
			return true;
		}
		if (sz_s2u(size_max) != size_max) {
			return true;
		}
		prev_max = size_max;
	}
	/*
	 * The HPA serves every small size and large sizes up to
	 * slab_max_alloc, so the sets must cover at least that much.
	 */
	size_t coverage = slab_max_alloc > SC_LARGE_MINCLASS
	    ? slab_max_alloc : SC_LARGE_MINCLASS;
	if (prev_max < coverage) {
		return true;
	}
	return false;
}

bool
hpa_shard_sets_config_add(hpa_shard_sets_config_t *config, size_t size_start,
    size_t size_end, unsigned nshards, hpa_shard_set_pick_t pick) {
	if (config->nsets == HPA_MAX_SHARD_SETS) {
		return true;
	}
	size_t expected = config->nsets == 0
	    ? 1 : config->sets[config->nsets - 1].size_max + 1;
	if (size_start != expected || size_end < size_start) {
		return true;
	}
	config->sets[config->nsets].size_max = size_end;
	config->sets[config->nsets].nshards = nshards;
	config->sets[config->nsets].pick = pick;
	config->nsets++;
	return false;
}

bool
hpa_shard_set_pick_from_name(
    const char *name, size_t len, hpa_shard_set_pick_t *pick) {
	for (unsigned i = 0; i < hpa_shard_set_pick_limit; i++) {
		const char *candidate = hpa_shard_set_pick_type_names[i];
		if (strlen(candidate) == len
		    && strncmp(candidate, name, len) == 0) {
			*pick = (hpa_shard_set_pick_t)i;
			return false;
		}
	}
	return true;
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
	 * half-built set for the fork hooks or lookup to touch.  Slot arrays
	 * already taken from base are reclaimed only with the base.
	 */
	memset(sets, 0, sizeof(*sets));
	sets->base = base;
	sets->central = central;
	sets->emap = emap;
	sets->edata_cache = edata_cache;

	for (unsigned i = 0; i < config->nsets; i++) {
		hpa_shard_set_t *shard_set = &sets->sets[i];
		shard_set->size_max = config->sets[i].size_max;
		shard_set->sets = sets;
		shard_set->nshards = config->sets[i].nshards;
		shard_set->pick = config->sets[i].pick;
		shard_set->opts = *opts;
		shard_set->sec_opts = *sec_opts;
		atomic_store_u(&shard_set->rr_next, 0, ATOMIC_RELAXED);
		shard_set->shards = (atomic_p_t *)base_alloc(tsdn, base,
		    shard_set->nshards * sizeof(atomic_p_t), CACHELINE);
		if (shard_set->shards == NULL) {
			return true;
		}
		for (unsigned j = 0; j < shard_set->nshards; j++) {
			atomic_store_p(&shard_set->shards[j], NULL,
			    ATOMIC_RELAXED);
		}
		if (malloc_mutex_init(&shard_set->init_mtx,
		    "hpa_shard_set_init", WITNESS_RANK_HPA_SHARD_SET,
		    malloc_mutex_rank_exclusive)) {
			return true;
		}
	}

	sets->nsets = config->nsets;
	sets->initialized = true;
	return false;
}

hpa_shard_t *
hpa_shard_set_pick_shard(
    tsdn_t *tsdn, hpa_shard_set_t *shard_set, unsigned affinity) {
	assert(shard_set->nshards > 0);

	unsigned idx;
	switch (shard_set->pick) {
	case hpa_shard_set_pick_arena:
		idx = affinity % shard_set->nshards;
		break;
	case hpa_shard_set_pick_roundrobin:
		idx = atomic_fetch_add_u(&shard_set->rr_next, 1, ATOMIC_RELAXED)
		    % shard_set->nshards;
		break;
	default:
		not_reached();
	}

	assert(idx < shard_set->nshards);
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &shard_set->shards[idx], ATOMIC_ACQUIRE);
	if (shard != NULL) {
		return shard;
	}

	malloc_mutex_lock(tsdn, &shard_set->init_mtx);
	shard = (hpa_shard_t *)atomic_load_p(&shard_set->shards[idx],
	    ATOMIC_RELAXED);
	if (shard == NULL) {
		hpa_shard_sets_t *sets = shard_set->sets;
		shard = (hpa_shard_t *)base_alloc(tsdn, sets->base,
		    sizeof(hpa_shard_t), CACHELINE);
		/* The slot index doubles as the shard's set-local owner id. */
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
	}
}
