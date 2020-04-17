#define JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* Data. */

bool	opt_tcache = true;
ssize_t	opt_lg_tcache_max = LG_TCACHE_MAXCLASS_DEFAULT;

cache_bin_info_t	*tcache_bin_info;

/* Total stack size required (per tcache).  Include the padding above. */
static size_t tcache_bin_alloc_size;
static size_t tcache_bin_alloc_alignment;

unsigned		nhbins;
size_t			tcache_maxclass;

tcaches_t		*tcaches;

/* Index of first element within tcaches that has never been used. */
static unsigned		tcaches_past;

/* Head of singly linked list tracking available tcaches elements. */
static tcaches_t	*tcaches_avail;

/* Protects tcaches{,_past,_avail}. */
static malloc_mutex_t	tcaches_mtx;

/******************************************************************************/

size_t
tcache_salloc(tsdn_t *tsdn, const void *ptr) {
	return arena_salloc(tsdn, ptr);
}

uint64_t
tcache_gc_new_event_wait(tsd_t *tsd) {
	return TCACHE_GC_INCR_BYTES;
}

uint64_t
tcache_gc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

uint64_t
tcache_gc_dalloc_new_event_wait(tsd_t *tsd) {
	return TCACHE_GC_INCR_BYTES;
}

uint64_t
tcache_gc_dalloc_postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

static void
tcache_event(tsd_t *tsd) {
	tcache_t *tcache = tcache_get(tsd);
	if (tcache == NULL) {
		return;
	}

	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get(tsd);
	szind_t binind = tcache_slow->next_gc_bin;
	bool is_small = (binind < SC_NBINS);
	cache_bin_t *cache_bin = &tcache->bins[binind];

	cache_bin_sz_t low_water = cache_bin_low_water_get(cache_bin,
	    &tcache_bin_info[binind]);
	cache_bin_sz_t ncached = cache_bin_ncached_get(cache_bin,
	    &tcache_bin_info[binind]);
	if (low_water > 0) {
		/*
		 * Flush (ceiling) 3/4 of the objects below the low water mark.
		 */
		if (is_small) {
			assert(!tcache_slow->bin_refilled[binind]);
			tcache_bin_flush_small(tsd, tcache, cache_bin, binind,
			    ncached - low_water + (low_water >> 2));
			/*
			 * Reduce fill count by 2X.  Limit lg_fill_div such that
			 * the fill count is always at least 1.
			 */
			if ((cache_bin_info_ncached_max(
			    &tcache_bin_info[binind]) >>
			    (tcache_slow->lg_fill_div[binind] + 1)) >= 1) {
				tcache_slow->lg_fill_div[binind]++;
			}
		} else {
			tcache_bin_flush_large(tsd, tcache, cache_bin, binind,
			     ncached - low_water + (low_water >> 2));
		}
	} else if (is_small && tcache_slow->bin_refilled[binind]) {
		assert(low_water == 0);
		/*
		 * Increase fill count by 2X for small bins.  Make sure
		 * lg_fill_div stays greater than 0.
		 */
		if (tcache_slow->lg_fill_div[binind] > 1) {
			tcache_slow->lg_fill_div[binind]--;
		}
		tcache_slow->bin_refilled[binind] = false;
	}
	cache_bin_low_water_set(cache_bin);

	tcache_slow->next_gc_bin++;
	if (tcache_slow->next_gc_bin == nhbins) {
		tcache_slow->next_gc_bin = 0;
	}
}

void
tcache_gc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void
tcache_gc_dalloc_event_handler(tsd_t *tsd, uint64_t elapsed) {
	assert(elapsed == TE_INVALID_ELAPSED);
	tcache_event(tsd);
}

void *
tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena,
    tcache_t *tcache, cache_bin_t *cache_bin, szind_t binind,
    bool *tcache_success) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	void *ret;

	assert(tcache_slow->arena != NULL);
	unsigned nfill = cache_bin_info_ncached_max(&tcache_bin_info[binind])
	    >> tcache_slow->lg_fill_div[binind];
	arena_cache_bin_fill_small(tsdn, arena, cache_bin,
	    &tcache_bin_info[binind], binind, nfill);
	tcache_slow->bin_refilled[binind] = true;
	ret = cache_bin_alloc(cache_bin, tcache_success);

	return ret;
}

/* Enabled with --enable-extra-size-check. */
static void
tbin_edatas_lookup_size_check(tsd_t *tsd, cache_bin_ptr_array_t *arr,
    szind_t binind, size_t nflush, edata_t **edatas) {
	/* Avoids null-checking tsdn in the loop below. */
	util_assume(tsd != NULL);

	/*
	 * Verify that the items in the tcache all have the correct size; this
	 * is useful for catching sized deallocation bugs, also to fail early
	 * instead of corrupting metadata.  Since this can be turned on for opt
	 * builds, avoid the branch in the loop.
	 */
	size_t szind_sum = binind * nflush;
	for (unsigned i = 0; i < nflush; i++) {
		emap_full_alloc_ctx_t full_alloc_ctx;
		emap_full_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global,
		    cache_bin_ptr_array_get(arr, i), &full_alloc_ctx);
		edatas[i] = full_alloc_ctx.edata;
		szind_sum -= full_alloc_ctx.szind;
	}

	if (szind_sum != 0) {
		safety_check_fail_sized_dealloc(false);
	}
}

JEMALLOC_ALWAYS_INLINE bool
tcache_bin_flush_match(edata_t *edata, unsigned cur_arena_ind,
    unsigned cur_binshard, bool small) {
	if (small) {
		return edata_arena_ind_get(edata) == cur_arena_ind
		    && edata_binshard_get(edata) == cur_binshard;
	} else {
		return edata_arena_ind_get(edata) == cur_arena_ind;
	}
}

JEMALLOC_ALWAYS_INLINE void
tcache_bin_flush_impl(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem, bool small) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	/*
	 * A couple lookup calls take tsdn; declare it once for convenience
	 * instead of calling tsd_tsdn(tsd) all the time.
	 */
	tsdn_t *tsdn = tsd_tsdn(tsd);

	if (small) {
		assert(binind < SC_NBINS);
	} else {
		assert(binind < nhbins);
	}
	cache_bin_sz_t ncached = cache_bin_ncached_get(cache_bin,
	    &tcache_bin_info[binind]);
	assert((cache_bin_sz_t)rem <= ncached);
	arena_t *tcache_arena = tcache_slow->arena;
	assert(tcache_arena != NULL);

	unsigned nflush = ncached - rem;
	/*
	 * Variable length array must have > 0 length; the last element is never
	 * touched (it's just included to satisfy the no-zero-length rule).
	 */
	VARIABLE_ARRAY(edata_t *, item_edata, nflush + 1);
	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nflush);

	cache_bin_init_ptr_array_for_flush(cache_bin, &tcache_bin_info[binind],
	    &ptrs, nflush);

	/* Look up edata once per item. */
	if (config_opt_safety_checks) {
		tbin_edatas_lookup_size_check(tsd, &ptrs, binind, nflush,
		    item_edata);
	} else {
		for (unsigned i = 0 ; i < nflush; i++) {
			item_edata[i] = emap_edata_lookup(tsd_tsdn(tsd),
			    &arena_emap_global,
			    cache_bin_ptr_array_get(&ptrs, i));
		}
	}

	/*
	 * The slabs where we freed the last remaining object in the slab (and
	 * so need to free the slab itself).
	 * Used only if small == true.
	 */
	unsigned dalloc_count = 0;
	VARIABLE_ARRAY(edata_t *, dalloc_slabs, nflush + 1);

	/*
	 * We're about to grab a bunch of locks.  If one of them happens to be
	 * the one guarding the arena-level stats counters we flush our
	 * thread-local ones to, we do so under one critical section.
	 */
	bool merged_stats = false;
	while (nflush > 0) {
		/* Lock the arena, or bin, associated with the first object. */
		edata_t *edata = item_edata[0];
		unsigned cur_arena_ind = edata_arena_ind_get(edata);
		arena_t *cur_arena = arena_get(tsdn, cur_arena_ind, false);

		/*
		 * These assignments are always overwritten when small is true,
		 * and their values are always ignored when small is false, but
		 * to avoid the technical UB when we pass them as parameters, we
		 * need to intialize them.
		 */
		unsigned cur_binshard = 0;
		bin_t *cur_bin = NULL;
		if (small) {
			cur_binshard = edata_binshard_get(edata);
			cur_bin = &cur_arena->bins[binind].bin_shards[
			    cur_binshard];
			assert(cur_binshard < bin_infos[binind].n_shards);
		}

		if (small) {
			malloc_mutex_lock(tsdn, &cur_bin->lock);
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_lock(tsdn, &cur_arena->large_mtx);
		}

		/*
		 * If we acquired the right lock and have some stats to flush,
		 * flush them.
		 */
		if (config_stats && tcache_arena == cur_arena
		    && !merged_stats) {
			merged_stats = true;
			if (small) {
				cur_bin->stats.nflushes++;
				cur_bin->stats.nrequests +=
				    cache_bin->tstats.nrequests;
				cache_bin->tstats.nrequests = 0;
			} else {
				arena_stats_large_flush_nrequests_add(tsdn,
				    &tcache_arena->stats, binind,
				    cache_bin->tstats.nrequests);
				cache_bin->tstats.nrequests = 0;
			}
		}

		/*
		 * Large allocations need special prep done.  Afterwards, we can
		 * drop the large lock.
		 */
		if (!small) {
			for (unsigned i = 0; i < nflush; i++) {
				void *ptr = cache_bin_ptr_array_get(&ptrs, i);
				edata = item_edata[i];
				assert(ptr != NULL && edata != NULL);

				if (tcache_bin_flush_match(edata, cur_arena_ind,
				    cur_binshard, small)) {
					large_dalloc_prep_locked(tsdn,
					    edata);
				}
			}
		}
		if (!small && !arena_is_auto(cur_arena)) {
			malloc_mutex_unlock(tsdn, &cur_arena->large_mtx);
		}

		/* Deallocate whatever we can. */
		unsigned ndeferred = 0;
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = cache_bin_ptr_array_get(&ptrs, i);
			edata = item_edata[i];
			assert(ptr != NULL && edata != NULL);
			if (!tcache_bin_flush_match(edata, cur_arena_ind,
			    cur_binshard, small)) {
				/*
				 * The object was allocated either via a
				 * different arena, or a different bin in this
				 * arena.  Either way, stash the object so that
				 * it can be handled in a future pass.
				 */
				cache_bin_ptr_array_set(&ptrs, ndeferred, ptr);
				item_edata[ndeferred] = edata;
				ndeferred++;
				continue;
			}
			if (small) {
				if (arena_dalloc_bin_locked(tsdn, cur_arena,
				    cur_bin, binind, edata, ptr)) {
					dalloc_slabs[dalloc_count] = edata;
					dalloc_count++;
				}
			} else {
				large_dalloc_finish(tsdn, edata);
			}
		}

		if (small) {
			malloc_mutex_unlock(tsdn, &cur_bin->lock);
		}
		arena_decay_ticks(tsdn, cur_arena, nflush - ndeferred);
		nflush = ndeferred;
	}

	/* Handle all deferred slab dalloc. */
	assert(small || dalloc_count == 0);
	for (unsigned i = 0; i < dalloc_count; i++) {
		edata_t *slab = dalloc_slabs[i];
		arena_slab_dalloc(tsdn, arena_get_from_edata(slab), slab);

	}

	if (config_stats && !merged_stats) {
		if (small) {
			/*
			 * The flush loop didn't happen to flush to this
			 * thread's arena, so the stats didn't get merged.
			 * Manually do so now.
			 */
			unsigned binshard;
			bin_t *bin = arena_bin_choose_lock(tsdn, tcache_arena,
			    binind, &binshard);
			bin->stats.nflushes++;
			bin->stats.nrequests += cache_bin->tstats.nrequests;
			cache_bin->tstats.nrequests = 0;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &tcache_arena->stats, binind,
			    cache_bin->tstats.nrequests);
			cache_bin->tstats.nrequests = 0;
		}
	}

	cache_bin_finish_flush(cache_bin, &tcache_bin_info[binind], &ptrs,
	    ncached - rem);
}

void
tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_impl(tsd, tcache, cache_bin, binind, rem, true);
}

void
tcache_bin_flush_large(tsd_t *tsd, tcache_t *tcache, cache_bin_t *cache_bin,
    szind_t binind, unsigned rem) {
	tcache_bin_flush_impl(tsd, tcache, cache_bin, binind, rem, false);
}

void
tcache_arena_associate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	assert(tcache_slow->arena == NULL);
	tcache_slow->arena = arena;

	if (config_stats) {
		/* Link into list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);

		ql_elm_new(tcache_slow, link);
		ql_tail_insert(&arena->tcache_ql, tcache_slow, link);
		cache_bin_array_descriptor_init(
		    &tcache_slow->cache_bin_array_descriptor, tcache->bins);
		ql_tail_insert(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);

		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

static void
tcache_arena_dissociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache) {
	arena_t *arena = tcache_slow->arena;
	assert(arena != NULL);
	if (config_stats) {
		/* Unlink from list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		if (config_debug) {
			bool in_ql = false;
			tcache_slow_t *iter;
			ql_foreach(iter, &arena->tcache_ql, link) {
				if (iter == tcache_slow) {
					in_ql = true;
					break;
				}
			}
			assert(in_ql);
		}
		ql_remove(&arena->tcache_ql, tcache_slow, link);
		ql_remove(&arena->cache_bin_array_descriptor_ql,
		    &tcache_slow->cache_bin_array_descriptor, link);
		tcache_stats_merge(tsdn, tcache_slow->tcache, arena);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
	tcache_slow->arena = NULL;
}

void
tcache_arena_reassociate(tsdn_t *tsdn, tcache_slow_t *tcache_slow,
    tcache_t *tcache, arena_t *arena) {
	tcache_arena_dissociate(tsdn, tcache_slow, tcache);
	tcache_arena_associate(tsdn, tcache_slow, tcache, arena);
}

bool
tsd_tcache_enabled_data_init(tsd_t *tsd) {
	/* Called upon tsd initialization. */
	tsd_tcache_enabled_set(tsd, opt_tcache);
	tsd_slow_update(tsd);

	if (opt_tcache) {
		/* Trigger tcache init. */
		tsd_tcache_data_init(tsd);
	}

	return false;
}

static void
tcache_init(tsd_t *tsd, tcache_slow_t *tcache_slow, tcache_t *tcache,
    void *mem) {
	tcache->tcache_slow = tcache_slow;
	tcache_slow->tcache = tcache;

	memset(&tcache_slow->link, 0, sizeof(ql_elm(tcache_t)));
	tcache_slow->next_gc_bin = 0;
	tcache_slow->arena = NULL;
	tcache_slow->dyn_alloc = mem;

	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	memset(tcache->bins, 0, sizeof(cache_bin_t) * nhbins);

	size_t cur_offset = 0;
	cache_bin_preincrement(tcache_bin_info, nhbins, mem,
	    &cur_offset);
	for (unsigned i = 0; i < nhbins; i++) {
		if (i < SC_NBINS) {
			tcache_slow->lg_fill_div[i] = 1;
			tcache_slow->bin_refilled[i] = false;
		}
		cache_bin_t *cache_bin = &tcache->bins[i];
		cache_bin_init(cache_bin, &tcache_bin_info[i], mem,
		    &cur_offset);
	}
	cache_bin_postincrement(tcache_bin_info, nhbins, mem,
	    &cur_offset);
	/* Sanity check that the whole stack is used. */
	assert(cur_offset == tcache_bin_alloc_size);
}

/* Initialize auto tcache (embedded in TSD). */
bool
tsd_tcache_data_init(tsd_t *tsd) {
	tcache_slow_t *tcache_slow = tsd_tcache_slowp_get_unsafe(tsd);
	tcache_t *tcache = tsd_tcachep_get_unsafe(tsd);

	assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
	size_t alignment = tcache_bin_alloc_alignment;
	size_t size = sz_sa2u(tcache_bin_alloc_size, alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, alignment, true, NULL,
	    true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return true;
	}

	tcache_init(tsd, tcache_slow, tcache, mem);
	/*
	 * Initialization is a bit tricky here.  After malloc init is done, all
	 * threads can rely on arena_choose and associate tcache accordingly.
	 * However, the thread that does actual malloc bootstrapping relies on
	 * functional tsd, and it can only rely on a0.  In that case, we
	 * associate its tcache to a0 temporarily, and later on
	 * arena_choose_hard() will re-associate properly.
	 */
	tcache_slow->arena = NULL;
	arena_t *arena;
	if (!malloc_initialized()) {
		/* If in initialization, assign to a0. */
		arena = arena_get(tsd_tsdn(tsd), 0, false);
		tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
		    arena);
	} else {
		arena = arena_choose(tsd, NULL);
		/* This may happen if thread.tcache.enabled is used. */
		if (tcache_slow->arena == NULL) {
			tcache_arena_associate(tsd_tsdn(tsd), tcache_slow,
			    tcache, arena);
		}
	}
	assert(arena == tcache_slow->arena);

	return false;
}

/* Created manual tcache for tcache.create mallctl. */
tcache_t *
tcache_create_explicit(tsd_t *tsd) {
	/*
	 * We place the cache bin stacks, then the tcache_t, then a pointer to
	 * the beginning of the whole allocation (for freeing).  The makes sure
	 * the cache bins have the requested alignment.
	 */
	size_t size = tcache_bin_alloc_size + sizeof(tcache_t)
	    + sizeof(tcache_slow_t);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	size = sz_sa2u(size, tcache_bin_alloc_alignment);

	void *mem = ipallocztm(tsd_tsdn(tsd), size, tcache_bin_alloc_alignment,
	    true, NULL, true, arena_get(TSDN_NULL, 0, true));
	if (mem == NULL) {
		return NULL;
	}
	tcache_t *tcache = (void *)((uintptr_t)mem + tcache_bin_alloc_size);
	tcache_slow_t *tcache_slow =
	    (void *)((uintptr_t)mem + tcache_bin_alloc_size + sizeof(tcache_t));
	tcache_init(tsd, tcache_slow, tcache, mem);

	tcache_arena_associate(tsd_tsdn(tsd), tcache_slow, tcache,
	    arena_ichoose(tsd, NULL));

	return tcache;
}

static void
tcache_flush_cache(tsd_t *tsd, tcache_t *tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	assert(tcache_slow->arena != NULL);

	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (i < SC_NBINS) {
			tcache_bin_flush_small(tsd, tcache, cache_bin, i, 0);
		} else {
			tcache_bin_flush_large(tsd, tcache, cache_bin, i, 0);
		}
		if (config_stats) {
			assert(cache_bin->tstats.nrequests == 0);
		}
	}
}

void
tcache_flush(tsd_t *tsd) {
	assert(tcache_available(tsd));
	tcache_flush_cache(tsd, tsd_tcachep_get(tsd));
}

static void
tcache_destroy(tsd_t *tsd, tcache_t *tcache, bool tsd_tcache) {
	tcache_slow_t *tcache_slow = tcache->tcache_slow;
	tcache_flush_cache(tsd, tcache);
	arena_t *arena = tcache_slow->arena;
	tcache_arena_dissociate(tsd_tsdn(tsd), tcache_slow, tcache);

	if (tsd_tcache) {
		cache_bin_t *cache_bin = &tcache->bins[0];
		cache_bin_assert_empty(cache_bin, &tcache_bin_info[0]);
	}
	idalloctm(tsd_tsdn(tsd), tcache_slow->dyn_alloc, NULL, NULL, true,
	    true);

	/*
	 * The deallocation and tcache flush above may not trigger decay since
	 * we are on the tcache shutdown path (potentially with non-nominal
	 * tsd).  Manually trigger decay to avoid pathological cases.  Also
	 * include arena 0 because the tcache array is allocated from it.
	 */
	arena_decay(tsd_tsdn(tsd), arena_get(tsd_tsdn(tsd), 0, false),
	    false, false);

	if (arena_nthreads_get(arena, false) == 0 &&
	    !background_thread_enabled()) {
		/* Force purging when no threads assigned to the arena anymore. */
		arena_decay(tsd_tsdn(tsd), arena, false, true);
	} else {
		arena_decay(tsd_tsdn(tsd), arena, false, false);
	}
}

/* For auto tcache (embedded in TSD) only. */
void
tcache_cleanup(tsd_t *tsd) {
	tcache_t *tcache = tsd_tcachep_get(tsd);
	if (!tcache_available(tsd)) {
		assert(tsd_tcache_enabled_get(tsd) == false);
		assert(cache_bin_still_zero_initialized(&tcache->bins[0]));
		return;
	}
	assert(tsd_tcache_enabled_get(tsd));
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));

	tcache_destroy(tsd, tcache, true);
	if (config_debug) {
		/*
		 * For debug testing only, we want to pretend we're still in the
		 * zero-initialized state.
		 */
		memset(tcache->bins, 0, sizeof(cache_bin_t) * nhbins);
	}
}

void
tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	cassert(config_stats);

	/* Merge and reset tcache stats. */
	for (unsigned i = 0; i < nhbins; i++) {
		cache_bin_t *cache_bin = &tcache->bins[i];
		if (i < SC_NBINS) {
			unsigned binshard;
			bin_t *bin = arena_bin_choose_lock(tsdn, arena, i,
			    &binshard);
			bin->stats.nrequests += cache_bin->tstats.nrequests;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_flush_nrequests_add(tsdn,
			    &arena->stats, i, cache_bin->tstats.nrequests);
		}
		cache_bin->tstats.nrequests = 0;
	}
}

static bool
tcaches_create_prep(tsd_t *tsd, base_t *base) {
	bool err;

	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches == NULL) {
		tcaches = base_alloc(tsd_tsdn(tsd), base,
		    sizeof(tcache_t *) * (MALLOCX_TCACHE_MAX+1), CACHELINE);
		if (tcaches == NULL) {
			err = true;
			goto label_return;
		}
	}

	if (tcaches_avail == NULL && tcaches_past > MALLOCX_TCACHE_MAX) {
		err = true;
		goto label_return;
	}

	err = false;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	return err;
}

bool
tcaches_create(tsd_t *tsd, base_t *base, unsigned *r_ind) {
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);

	bool err;

	if (tcaches_create_prep(tsd, base)) {
		err = true;
		goto label_return;
	}

	tcache_t *tcache = tcache_create_explicit(tsd);
	if (tcache == NULL) {
		err = true;
		goto label_return;
	}

	tcaches_t *elm;
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcaches_avail != NULL) {
		elm = tcaches_avail;
		tcaches_avail = tcaches_avail->next;
		elm->tcache = tcache;
		*r_ind = (unsigned)(elm - tcaches);
	} else {
		elm = &tcaches[tcaches_past];
		elm->tcache = tcache;
		*r_ind = tcaches_past;
		tcaches_past++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);

	err = false;
label_return:
	witness_assert_depth(tsdn_witness_tsdp_get(tsd_tsdn(tsd)), 0);
	return err;
}

static tcache_t *
tcaches_elm_remove(tsd_t *tsd, tcaches_t *elm, bool allow_reinit) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (elm->tcache == NULL) {
		return NULL;
	}
	tcache_t *tcache = elm->tcache;
	if (allow_reinit) {
		elm->tcache = TCACHES_ELM_NEED_REINIT;
	} else {
		elm->tcache = NULL;
	}

	if (tcache == TCACHES_ELM_NEED_REINIT) {
		return NULL;
	}
	return tcache;
}

void
tcaches_flush(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcache_t *tcache = tcaches_elm_remove(tsd, &tcaches[ind], true);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		/* Destroy the tcache; recreate in tcaches_get() if needed. */
		tcache_destroy(tsd, tcache, false);
	}
}

void
tcaches_destroy(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcaches_t *elm = &tcaches[ind];
	tcache_t *tcache = tcaches_elm_remove(tsd, elm, false);
	elm->next = tcaches_avail;
	tcaches_avail = elm;
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache, false);
	}
}

bool
tcache_boot(tsdn_t *tsdn, base_t *base) {
	/* If necessary, clamp opt_lg_tcache_max. */
	if (opt_lg_tcache_max < 0 || (ZU(1) << opt_lg_tcache_max) <
	    SC_SMALL_MAXCLASS) {
		tcache_maxclass = SC_SMALL_MAXCLASS;
	} else {
		tcache_maxclass = (ZU(1) << opt_lg_tcache_max);
	}

	if (malloc_mutex_init(&tcaches_mtx, "tcaches", WITNESS_RANK_TCACHES,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	nhbins = sz_size2index(tcache_maxclass) + 1;

	/* Initialize tcache_bin_info. */
	tcache_bin_info = (cache_bin_info_t *)base_alloc(tsdn, base,
	    nhbins * sizeof(cache_bin_info_t), CACHELINE);
	if (tcache_bin_info == NULL) {
		return true;
	}
	unsigned i, ncached_max;
	for (i = 0; i < SC_NBINS; i++) {
		if ((bin_infos[i].nregs << 1) <= TCACHE_NSLOTS_SMALL_MIN) {
			ncached_max = TCACHE_NSLOTS_SMALL_MIN;
		} else if ((bin_infos[i].nregs << 1) <=
		    TCACHE_NSLOTS_SMALL_MAX) {
			ncached_max = bin_infos[i].nregs << 1;
		} else {
			ncached_max = TCACHE_NSLOTS_SMALL_MAX;
		}
		cache_bin_info_init(&tcache_bin_info[i], ncached_max);
	}
	for (; i < nhbins; i++) {
		cache_bin_info_init(&tcache_bin_info[i], TCACHE_NSLOTS_LARGE);
	}
	cache_bin_info_compute_alloc(tcache_bin_info, i, &tcache_bin_alloc_size,
	    &tcache_bin_alloc_alignment);

	return false;
}

void
tcache_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &tcaches_mtx);
}

void
tcache_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &tcaches_mtx);
}

void
tcache_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &tcaches_mtx);
}

void tcache_assert_initialized(tcache_t *tcache) {
	assert(!cache_bin_still_zero_initialized(&tcache->bins[0]));
}
