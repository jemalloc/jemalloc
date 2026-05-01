#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sec.h"
#include "jemalloc/internal/jemalloc_probe.h"

static bool
sec_bin_init(sec_bin_t *bin) {
	atomic_store_zu(&bin->bytes_cur, 0, ATOMIC_RELAXED);
	atomic_store_zu(&bin->ndalloc_flush, 0, ATOMIC_RELAXED);
	atomic_store_zu(&bin->nmisses, 0, ATOMIC_RELAXED);
	atomic_store_zu(&bin->nhits, 0, ATOMIC_RELAXED);
	atomic_store_zu(&bin->ndalloc_noflush, 0, ATOMIC_RELAXED);
	atomic_store_zu(&bin->noverfills, 0, ATOMIC_RELAXED);
	edata_list_active_init(&bin->freelist);
	bool err = malloc_mutex_init(&bin->mtx, "sec_bin", WITNESS_RANK_SEC_BIN,
	    malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	return false;
}

bool
sec_init(tsdn_t *tsdn, sec_t *sec, base_t *base, const sec_opts_t *opts) {
	sec->opts = *opts;
	if (opts->nshards == 0) {
		return false;
	}
	assert(opts->max_alloc >= PAGE);

	/*
	 * Same as tcache, sec do not cache allocs/dallocs larger than
	 * USIZE_GROW_SLOW_THRESHOLD because the usize above this increases
	 * by PAGE and the number of usizes is too large.
	 */
	assert(opts->max_alloc <= USIZE_GROW_SLOW_THRESHOLD);

	size_t   max_alloc = PAGE_FLOOR(opts->max_alloc);
	pszind_t npsizes = sz_psz2ind(max_alloc) + 1;

	size_t ntotal_bins = opts->nshards * (size_t)npsizes;
	size_t sz_bins = sizeof(sec_bin_t) * ntotal_bins;
	void  *dynalloc = base_alloc(tsdn, base, sz_bins, CACHELINE);
	if (dynalloc == NULL) {
		return true;
	}
	sec->bins = (sec_bin_t *)dynalloc;
	for (pszind_t j = 0; j < ntotal_bins; j++) {
		if (sec_bin_init(&sec->bins[j])) {
			return true;
		}
	}
	sec->npsizes = npsizes;

	return false;
}

void
sec_shard_init(tsd_t *tsd, size_t nshards, uint8_t *shardp) {
	/*
	 * When nshards <= 1 the default 0 stored in TSD_INITIALIZER is
	 * already correct, so leave it alone.
	 * Use the trick from Daniel Lemire's "A fast alternative to the
	 * modulo reduction": multiply a 32-bit random by nshards in 64 bits
	 * and shift back, avoiding an actual modulo.
	 */
	if (nshards <= 1) {
		return;
	}
	uint64_t rand32 = prng_lg_range_u64(tsd_prng_statep_get(tsd), 32);
	uint32_t idx = (uint32_t)((rand32 * (uint64_t)nshards) >> 32);
	assert(idx < (uint32_t)nshards);
	*shardp = (uint8_t)idx;
}

static sec_bin_t *
sec_bin_pick(sec_t *sec, uint8_t shard, pszind_t pszind) {
	assert(shard < sec->opts.nshards);
	size_t ind = (size_t)shard * sec->npsizes + pszind;
	assert(ind < sec->npsizes * sec->opts.nshards);
	return &sec->bins[ind];
}

static edata_t *
sec_bin_alloc_locked(tsdn_t *tsdn, sec_t *sec, sec_bin_t *bin, size_t size) {
	malloc_mutex_assert_owner(tsdn, &bin->mtx);

	edata_t *edata = edata_list_active_first(&bin->freelist);
	if (edata != NULL) {
		assert(!edata_list_active_empty(&bin->freelist));
		edata_list_active_remove(&bin->freelist, edata);
		size_t sz = edata_size_get(edata);
		size_t bytes_cur = atomic_load_zu(
		    &bin->bytes_cur, ATOMIC_RELAXED);
		assert(sz <= bytes_cur && sz > 0);
		bytes_cur -= sz;
		atomic_store_zu(&bin->bytes_cur, bytes_cur, ATOMIC_RELAXED);
		atomic_load_add_store_zu(&bin->nhits, 1);
	}
	return edata;
}

static edata_t *
sec_multishard_trylock_alloc(
    tsdn_t *tsdn, sec_t *sec, size_t size, pszind_t pszind, uint8_t shard) {
	assert(sec->opts.nshards > 0);

	uint8_t    cur_shard = shard;
	sec_bin_t *bin;
	for (size_t i = 0; i < sec->opts.nshards; ++i) {
		bin = sec_bin_pick(sec, cur_shard, pszind);
		if (!malloc_mutex_trylock(tsdn, &bin->mtx)) {
			edata_t *edata = sec_bin_alloc_locked(
			    tsdn, sec, bin, size);
			malloc_mutex_unlock(tsdn, &bin->mtx);
			if (edata != NULL) {
				JE_USDT(sec_alloc, 5, sec, bin, edata, size,
				    /* frequent_reuse */ 1);
				return edata;
			}
		}
		cur_shard++;
		if (cur_shard == sec->opts.nshards) {
			cur_shard = 0;
		}
	}
	/*
	 * TODO: Benchmark whether it is worth blocking on all shards here before
	 * declaring a miss.  That could recover more remote-shard hits under
	 * contention, but it also changes the allocation latency policy.
	 */
	assert(cur_shard == shard);
	bin = sec_bin_pick(sec, cur_shard, pszind);
	malloc_mutex_lock(tsdn, &bin->mtx);
	edata_t *edata = sec_bin_alloc_locked(tsdn, sec, bin, size);
	if (edata == NULL) {
		/* Only now we know it is a miss. */
		atomic_load_add_store_zu(&bin->nmisses, 1);
	}
	malloc_mutex_unlock(tsdn, &bin->mtx);
	JE_USDT(sec_alloc, 5, sec, bin, edata, size, /* frequent_reuse */ 1);
	return edata;
}

edata_t *
sec_alloc(tsdn_t *tsdn, sec_t *sec, size_t size, uint8_t shard) {
	if (!sec_size_supported(sec, size)) {
		return NULL;
	}
	assert((size & PAGE_MASK) == 0);
	pszind_t pszind = sz_psz2ind(size);
	assert(pszind < sec->npsizes);

	/*
	 * If there's only one shard, skip the trylock optimization and
	 * go straight to the blocking lock.
	 */
	if (sec->opts.nshards == 1) {
		sec_bin_t *bin = sec_bin_pick(sec, /* shard */ 0, pszind);
		malloc_mutex_lock(tsdn, &bin->mtx);
		edata_t *edata = sec_bin_alloc_locked(tsdn, sec, bin, size);
		if (edata == NULL) {
			atomic_load_add_store_zu(&bin->nmisses, 1);
		}
		malloc_mutex_unlock(tsdn, &bin->mtx);
		JE_USDT(sec_alloc, 5, sec, bin, edata, size,
		    /* frequent_reuse */ 1);
		return edata;
	}
	return sec_multishard_trylock_alloc(tsdn, sec, size, pszind, shard);
}

static void
sec_bin_dalloc_locked(tsdn_t *tsdn, sec_t *sec, sec_bin_t *bin, size_t size,
    edata_list_active_t *dalloc_list) {
	malloc_mutex_assert_owner(tsdn, &bin->mtx);

	size_t bytes_cur = atomic_load_zu(&bin->bytes_cur, ATOMIC_RELAXED)
	    + size;
	edata_t *edata = edata_list_active_first(dalloc_list);
	assert(edata != NULL);
	edata_list_active_remove(dalloc_list, edata);
	JE_USDT(sec_dalloc, 3, sec, bin, edata);
	edata_list_active_prepend(&bin->freelist, edata);
	/* Single extent can be returned to SEC */
	assert(edata_list_active_empty(dalloc_list));

	if (bytes_cur <= sec->opts.max_bytes) {
		atomic_store_zu(&bin->bytes_cur, bytes_cur, ATOMIC_RELAXED);
		atomic_load_add_store_zu(&bin->ndalloc_noflush, 1);
		return;
	}
	atomic_load_add_store_zu(&bin->ndalloc_flush, 1);
	/* we want to flush 1/4 of max_bytes */
	size_t bytes_target = sec->opts.max_bytes - (sec->opts.max_bytes >> 2);
	while (bytes_cur > bytes_target
	    && !edata_list_active_empty(&bin->freelist)) {
		edata_t *cur = edata_list_active_last(&bin->freelist);
		size_t   sz = edata_size_get(cur);
		assert(sz <= bytes_cur && sz > 0);
		bytes_cur -= sz;
		edata_list_active_remove(&bin->freelist, cur);
		edata_list_active_append(dalloc_list, cur);
	}
	atomic_store_zu(&bin->bytes_cur, bytes_cur, ATOMIC_RELAXED);
}

static void
sec_multishard_trylock_dalloc(tsdn_t *tsdn, sec_t *sec, size_t size,
    pszind_t pszind, edata_list_active_t *dalloc_list, uint8_t shard) {
	assert(sec->opts.nshards > 0);

	/* Try to dalloc in this threads bin first */
	uint8_t cur_shard = shard;
	for (size_t i = 0; i < sec->opts.nshards; ++i) {
		sec_bin_t *bin = sec_bin_pick(sec, cur_shard, pszind);
		if (!malloc_mutex_trylock(tsdn, &bin->mtx)) {
			sec_bin_dalloc_locked(
			    tsdn, sec, bin, size, dalloc_list);
			malloc_mutex_unlock(tsdn, &bin->mtx);
			return;
		}
		cur_shard++;
		if (cur_shard == sec->opts.nshards) {
			cur_shard = 0;
		}
	}
	/* No bin had alloc or had the extent */
	assert(cur_shard == shard);
	sec_bin_t *bin = sec_bin_pick(sec, cur_shard, pszind);
	malloc_mutex_lock(tsdn, &bin->mtx);
	sec_bin_dalloc_locked(tsdn, sec, bin, size, dalloc_list);
	malloc_mutex_unlock(tsdn, &bin->mtx);
}

void
sec_dalloc(tsdn_t *tsdn, sec_t *sec, edata_list_active_t *dalloc_list,
    uint8_t shard) {
	if (!sec_is_used(sec)) {
		return;
	}
	edata_t *edata = edata_list_active_first(dalloc_list);
	size_t   size = edata_size_get(edata);
	if (size > sec->opts.max_alloc) {
		return;
	}
	pszind_t pszind = sz_psz2ind(size);
	assert(pszind < sec->npsizes);

	/*
         * If there's only one shard, skip the trylock optimization and
	 * go straight to the blocking lock.
	 */
	if (sec->opts.nshards == 1) {
		sec_bin_t *bin = sec_bin_pick(sec, /* shard */ 0, pszind);
		malloc_mutex_lock(tsdn, &bin->mtx);
		sec_bin_dalloc_locked(tsdn, sec, bin, size, dalloc_list);
		malloc_mutex_unlock(tsdn, &bin->mtx);
		return;
	}
	sec_multishard_trylock_dalloc(tsdn, sec, size, pszind, dalloc_list,
	    shard);
}

void
sec_fill(tsdn_t *tsdn, sec_t *sec, size_t size, edata_list_active_t *result,
    size_t nallocs, uint8_t shard) {
	assert((size & PAGE_MASK) == 0);
	assert(sec->opts.nshards != 0 && size <= sec->opts.max_alloc);
	assert(nallocs > 0);

	pszind_t pszind = sz_psz2ind(size);
	assert(pszind < sec->npsizes);

	sec_bin_t *bin = sec_bin_pick(sec, shard, pszind);
	malloc_mutex_assert_not_owner(tsdn, &bin->mtx);
	malloc_mutex_lock(tsdn, &bin->mtx);
	size_t new_cached_bytes = nallocs * size;
	size_t bytes_cur = atomic_load_zu(&bin->bytes_cur, ATOMIC_RELAXED);
	if (bytes_cur + new_cached_bytes <= sec->opts.max_bytes) {
		assert(!edata_list_active_empty(result));
		edata_list_active_concat(&bin->freelist, result);
		atomic_store_zu(&bin->bytes_cur, bytes_cur + new_cached_bytes,
		    ATOMIC_RELAXED);
	} else {
		/*
		 * Unlikely case of many threads filling at the same time and
		 * going above max.
		 */
		atomic_load_add_store_zu(&bin->noverfills, 1);
		while (bytes_cur + size <= sec->opts.max_bytes) {
			edata_t *edata = edata_list_active_first(result);
			if (edata == NULL) {
				break;
			}
			edata_list_active_remove(result, edata);
			assert(size == edata_size_get(edata));
			edata_list_active_append(&bin->freelist, edata);
			bytes_cur += size;
		}
		atomic_store_zu(&bin->bytes_cur, bytes_cur, ATOMIC_RELAXED);
	}
	malloc_mutex_unlock(tsdn, &bin->mtx);
}

void
sec_flush(tsdn_t *tsdn, sec_t *sec, edata_list_active_t *to_flush) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		malloc_mutex_lock(tsdn, &bin->mtx);
		atomic_store_zu(&bin->bytes_cur, 0, ATOMIC_RELAXED);
		edata_list_active_concat(to_flush, &bin->freelist);
		malloc_mutex_unlock(tsdn, &bin->mtx);
	}
}

void
sec_stats_merge(tsdn_t *tsdn, sec_t *sec, sec_stats_t *stats) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t sum = 0;
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		sum += atomic_load_zu(&bin->bytes_cur, ATOMIC_RELAXED);
		stats->total.nmisses +=
		    atomic_load_zu(&bin->nmisses, ATOMIC_RELAXED);
		stats->total.nhits +=
		    atomic_load_zu(&bin->nhits, ATOMIC_RELAXED);
		stats->total.ndalloc_flush +=
		    atomic_load_zu(&bin->ndalloc_flush, ATOMIC_RELAXED);
		stats->total.ndalloc_noflush +=
		    atomic_load_zu(&bin->ndalloc_noflush, ATOMIC_RELAXED);
		stats->total.noverfills +=
		    atomic_load_zu(&bin->noverfills, ATOMIC_RELAXED);
	}
	stats->bytes += sum;
}

void
sec_mutex_stats_read(
    tsdn_t *tsdn, sec_t *sec, mutex_prof_data_t *mutex_prof_data) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		malloc_mutex_lock(tsdn, &bin->mtx);
		malloc_mutex_prof_accum(tsdn, mutex_prof_data, &bin->mtx);
		malloc_mutex_unlock(tsdn, &bin->mtx);
	}
}

void
sec_prefork2(tsdn_t *tsdn, sec_t *sec) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		malloc_mutex_prefork(tsdn, &bin->mtx);
	}
}

void
sec_postfork_parent(tsdn_t *tsdn, sec_t *sec) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		malloc_mutex_postfork_parent(tsdn, &bin->mtx);
	}
}

void
sec_postfork_child(tsdn_t *tsdn, sec_t *sec) {
	if (!sec_is_used(sec)) {
		return;
	}
	size_t ntotal_bins = sec->opts.nshards * sec->npsizes;
	for (pszind_t i = 0; i < ntotal_bins; i++) {
		sec_bin_t *bin = &sec->bins[i];
		malloc_mutex_postfork_child(tsdn, &bin->mtx);
	}
}
