#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/sec.h"

static edata_t *sec_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero);
static bool sec_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero);
static bool sec_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size);
static void sec_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata);

static void
sec_bin_init(sec_bin_t *bin) {
	bin->bytes_cur = 0;
	edata_list_active_init(&bin->freelist);
}

bool
sec_init(sec_t *sec, pai_t *fallback, size_t nshards, size_t alloc_max,
    size_t bytes_max) {
	if (nshards > SEC_NSHARDS_MAX) {
		nshards = SEC_NSHARDS_MAX;
	}
	for (size_t i = 0; i < nshards; i++) {
		sec_shard_t *shard = &sec->shards[i];
		bool err = malloc_mutex_init(&shard->mtx, "sec_shard",
		    WITNESS_RANK_SEC_SHARD, malloc_mutex_rank_exclusive);
		if (err) {
			return true;
		}
		shard->enabled = true;
		for (pszind_t j = 0; j < SEC_NPSIZES; j++) {
			sec_bin_init(&shard->bins[j]);
		}
		shard->bytes_cur = 0;
		shard->to_flush_next = 0;
	}
	sec->fallback = fallback;
	sec->alloc_max = alloc_max;
	if (sec->alloc_max > sz_pind2sz(SEC_NPSIZES - 1)) {
		sec->alloc_max = sz_pind2sz(SEC_NPSIZES - 1);
	}

	sec->bytes_max = bytes_max;
	sec->bytes_after_flush = bytes_max / 2;
	sec->nshards = nshards;

	/*
	 * Initialize these last so that an improper use of an SEC whose
	 * initialization failed will segfault in an easy-to-spot way.
	 */
	sec->pai.alloc = &sec_alloc;
	sec->pai.alloc_batch = &pai_alloc_batch_default;
	sec->pai.expand = &sec_expand;
	sec->pai.shrink = &sec_shrink;
	sec->pai.dalloc = &sec_dalloc;
	sec->pai.dalloc_batch = &pai_dalloc_batch_default;

	return false;
}

static sec_shard_t *
sec_shard_pick(tsdn_t *tsdn, sec_t *sec) {
	/*
	 * Eventually, we should implement affinity, tracking source shard using
	 * the edata_t's newly freed up fields.  For now, just randomly
	 * distribute across all shards.
	 */
	if (tsdn_null(tsdn)) {
		return &sec->shards[0];
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	uint8_t *idxp = tsd_sec_shardp_get(tsd);
	if (*idxp == (uint8_t)-1) {
		/*
		 * First use; initialize using the trick from Daniel Lemire's
		 * "A fast alternative to the modulo reduction.  Use a 64 bit
		 * number to store 32 bits, since we'll deliberately overflow
		 * when we multiply by the number of shards.
		 */
		uint64_t rand32 = prng_lg_range_u64(tsd_prng_statep_get(tsd), 32);
		uint32_t idx = (uint32_t)((rand32 * (uint64_t)sec->nshards) >> 32);
		assert(idx < (uint32_t)sec->nshards);
		*idxp = (uint8_t)idx;
	}
	return &sec->shards[*idxp];
}

static edata_t *
sec_shard_alloc_locked(tsdn_t *tsdn, sec_t *sec, sec_shard_t *shard,
    pszind_t pszind) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	if (!shard->enabled) {
		return NULL;
	}
	sec_bin_t *bin = &shard->bins[pszind];
	edata_t *edata = edata_list_active_first(&bin->freelist);
	if (edata != NULL) {
		edata_list_active_remove(&bin->freelist, edata);
		assert(edata_size_get(edata) <= bin->bytes_cur);
		bin->bytes_cur -= edata_size_get(edata);
		assert(edata_size_get(edata) <= shard->bytes_cur);
		shard->bytes_cur -= edata_size_get(edata);
	}
	return edata;
}

static edata_t *
sec_alloc(tsdn_t *tsdn, pai_t *self, size_t size, size_t alignment, bool zero) {
	assert((size & PAGE_MASK) == 0);

	sec_t *sec = (sec_t *)self;

	if (zero || alignment > PAGE || sec->nshards == 0
	    || size > sec->alloc_max) {
		return pai_alloc(tsdn, sec->fallback, size, alignment, zero);
	}
	pszind_t pszind = sz_psz2ind(size);
	sec_shard_t *shard = sec_shard_pick(tsdn, sec);
	malloc_mutex_lock(tsdn, &shard->mtx);
	edata_t *edata = sec_shard_alloc_locked(tsdn, sec, shard, pszind);
	malloc_mutex_unlock(tsdn, &shard->mtx);
	if (edata == NULL) {
		/*
		 * See the note in dalloc, below; really, we should add a
		 * batch_alloc method to the PAI and get more than one extent at
		 * a time.
		 */
		edata = pai_alloc(tsdn, sec->fallback, size, alignment, zero);
	}
	return edata;
}

static bool
sec_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size, bool zero) {
	sec_t *sec = (sec_t *)self;
	return pai_expand(tsdn, sec->fallback, edata, old_size, new_size, zero);
}

static bool
sec_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata, size_t old_size,
    size_t new_size) {
	sec_t *sec = (sec_t *)self;
	return pai_shrink(tsdn, sec->fallback, edata, old_size, new_size);
}

static void
sec_flush_all_locked(tsdn_t *tsdn, sec_t *sec, sec_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	shard->bytes_cur = 0;
	edata_list_active_t to_flush;
	edata_list_active_init(&to_flush);
	for (pszind_t i = 0; i < SEC_NPSIZES; i++) {
		sec_bin_t *bin = &shard->bins[i];
		bin->bytes_cur = 0;
		edata_list_active_concat(&to_flush, &bin->freelist);
	}

	/*
	 * Ordinarily we would try to avoid doing the batch deallocation while
	 * holding the shard mutex, but the flush_all pathways only happen when
	 * we're disabling the HPA or resetting the arena, both of which are
	 * rare pathways.
	 */
	pai_dalloc_batch(tsdn, sec->fallback, &to_flush);
}

static void
sec_flush_some_and_unlock(tsdn_t *tsdn, sec_t *sec, sec_shard_t *shard) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	edata_list_active_t to_flush;
	edata_list_active_init(&to_flush);
	while (shard->bytes_cur > sec->bytes_after_flush) {
		/* Pick a victim. */
		sec_bin_t *bin = &shard->bins[shard->to_flush_next];

		/* Update our victim-picking state. */
		shard->to_flush_next++;
		if (shard->to_flush_next == SEC_NPSIZES) {
			shard->to_flush_next = 0;
		}

		assert(shard->bytes_cur >= bin->bytes_cur);
		if (bin->bytes_cur != 0) {
			shard->bytes_cur -= bin->bytes_cur;
			bin->bytes_cur = 0;
			edata_list_active_concat(&to_flush, &bin->freelist);
		}
		/*
		 * Either bin->bytes_cur was 0, in which case we didn't touch
		 * the bin list but it should be empty anyways (or else we
		 * missed a bytes_cur update on a list modification), or it
		 * *was* 0 and we emptied it ourselves.  Either way, it should
		 * be empty now.
		 */
		assert(edata_list_active_empty(&bin->freelist));
	}

	malloc_mutex_unlock(tsdn, &shard->mtx);
	pai_dalloc_batch(tsdn, sec->fallback, &to_flush);
}

static void
sec_shard_dalloc_and_unlock(tsdn_t *tsdn, sec_t *sec, sec_shard_t *shard,
    edata_t *edata) {
	malloc_mutex_assert_owner(tsdn, &shard->mtx);
	assert(shard->bytes_cur <= sec->bytes_max);
	size_t size = edata_size_get(edata);
	pszind_t pszind = sz_psz2ind(size);
	/*
	 * Prepending here results in LIFO allocation per bin, which seems
	 * reasonable.
	 */
	sec_bin_t *bin = &shard->bins[pszind];
	edata_list_active_prepend(&bin->freelist, edata);
	bin->bytes_cur += size;
	shard->bytes_cur += size;
	if (shard->bytes_cur > sec->bytes_max) {
		/*
		 * We've exceeded the shard limit.  We make two nods in the
		 * direction of fragmentation avoidance: we flush everything in
		 * the shard, rather than one particular bin, and we hold the
		 * lock while flushing (in case one of the extents we flush is
		 * highly preferred from a fragmentation-avoidance perspective
		 * in the backing allocator).  This has the extra advantage of
		 * not requiring advanced cache balancing strategies.
		 */
		sec_flush_some_and_unlock(tsdn, sec, shard);
		malloc_mutex_assert_not_owner(tsdn, &shard->mtx);
	} else {
		malloc_mutex_unlock(tsdn, &shard->mtx);
	}
}

static void
sec_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata) {
	sec_t *sec = (sec_t *)self;
	if (sec->nshards == 0 || edata_size_get(edata) > sec->alloc_max) {
		pai_dalloc(tsdn, sec->fallback, edata);
		return;
	}
	sec_shard_t *shard = sec_shard_pick(tsdn, sec);
	malloc_mutex_lock(tsdn, &shard->mtx);
	if (shard->enabled) {
		sec_shard_dalloc_and_unlock(tsdn, sec, shard, edata);
	} else {
		malloc_mutex_unlock(tsdn, &shard->mtx);
		pai_dalloc(tsdn, sec->fallback, edata);
	}
}

void
sec_flush(tsdn_t *tsdn, sec_t *sec) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_lock(tsdn, &sec->shards[i].mtx);
		sec_flush_all_locked(tsdn, sec, &sec->shards[i]);
		malloc_mutex_unlock(tsdn, &sec->shards[i].mtx);
	}
}

void
sec_disable(tsdn_t *tsdn, sec_t *sec) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_lock(tsdn, &sec->shards[i].mtx);
		sec->shards[i].enabled = false;
		sec_flush_all_locked(tsdn, sec, &sec->shards[i]);
		malloc_mutex_unlock(tsdn, &sec->shards[i].mtx);
	}
}

void
sec_stats_merge(tsdn_t *tsdn, sec_t *sec, sec_stats_t *stats) {
	size_t sum = 0;
	for (size_t i = 0; i < sec->nshards; i++) {
		/*
		 * We could save these lock acquisitions by making bytes_cur
		 * atomic, but stats collection is rare anyways and we expect
		 * the number and type of stats to get more interesting.
		 */
		malloc_mutex_lock(tsdn, &sec->shards[i].mtx);
		sum += sec->shards[i].bytes_cur;
		malloc_mutex_unlock(tsdn, &sec->shards[i].mtx);
	}
	stats->bytes += sum;
}

void
sec_mutex_stats_read(tsdn_t *tsdn, sec_t *sec,
    mutex_prof_data_t *mutex_prof_data) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_lock(tsdn, &sec->shards[i].mtx);
		malloc_mutex_prof_accum(tsdn, mutex_prof_data,
		    &sec->shards[i].mtx);
		malloc_mutex_unlock(tsdn, &sec->shards[i].mtx);
	}
}

void
sec_prefork2(tsdn_t *tsdn, sec_t *sec) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_prefork(tsdn, &sec->shards[i].mtx);
	}
}

void
sec_postfork_parent(tsdn_t *tsdn, sec_t *sec) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_postfork_parent(tsdn, &sec->shards[i].mtx);
	}
}

void
sec_postfork_child(tsdn_t *tsdn, sec_t *sec) {
	for (size_t i = 0; i < sec->nshards; i++) {
		malloc_mutex_postfork_child(tsdn, &sec->shards[i].mtx);
	}
}
