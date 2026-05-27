#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/background_thread.h"
#include "jemalloc/internal/background_thread_inlines.h"
#include "jemalloc/internal/ctl_arena.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* arena.* and arenas.* ctl state. */

static ctl_arenas_t *ctl_arenas;

static void
ctl_accum_locked_u64(locked_u64_t *dst, locked_u64_t *src) {
	locked_inc_u64_unsynchronized(dst, locked_read_u64_unsynchronized(src));
}

static void
ctl_accum_atomic_zu(atomic_zu_t *dst, atomic_zu_t *src) {
	size_t cur_dst = atomic_load_zu(dst, ATOMIC_RELAXED);
	size_t cur_src = atomic_load_zu(src, ATOMIC_RELAXED);
	atomic_store_zu(dst, cur_dst + cur_src, ATOMIC_RELAXED);
}

/*
 * Historical compatibility for treating arena.<narenas> as the merged
 * all-arenas entry.  New code should use MALLCTL_ARENAS_ALL.
 *
 * `narenas` must be a snapshot of ctl_arenas->narenas taken while holding
 * ctl_mtx; see ctl_narenas_get.
 */
static bool
ctl_arena_ind_is_deprecated_all(size_t i, unsigned narenas) {
	return i == narenas;
}

/*
 * `narenas` must be a snapshot of ctl_arenas->narenas taken while holding
 * ctl_mtx; see ctl_narenas_get.
 */
static bool
ctl_arena_ind_is_all(size_t i, unsigned narenas) {
	return i == MALLCTL_ARENAS_ALL
	    || ctl_arena_ind_is_deprecated_all(i, narenas);
}

/*
 * ctl_arenas->narenas may grow concurrently (arena creation in
 * ctl_arena_init); all reads must happen while ctl_mtx is held.  This
 * helper centralizes that requirement.
 */
unsigned
ctl_narenas_get(tsdn_t *tsdn) {
	ctl_mtx_assert_held(tsdn);
	return ctl_arenas->narenas;
}

static unsigned
arenas_i2a_impl(size_t i, unsigned narenas, bool compat, bool validate) {
	unsigned a;

	switch (i) {
	case MALLCTL_ARENAS_ALL:
		a = 0;
		break;
	case MALLCTL_ARENAS_DESTROYED:
		a = 1;
		break;
	default:
		if (compat && ctl_arena_ind_is_deprecated_all(i, narenas)) {
			/*
			 * Provide deprecated backward compatibility for
			 * accessing the merged stats at index narenas rather
			 * than via MALLCTL_ARENAS_ALL.  This is scheduled for
			 * removal in 6.0.0.
			 */
			a = 0;
		} else if (validate && i >= narenas) {
			a = UINT_MAX;
		} else {
			/*
			 * This function should never be called for an index
			 * more than one past the range of indices that have
			 * initialized ctl data.
			 */
			assert(i < narenas || (!validate && i == narenas));
			a = (unsigned)i + 2;
		}
		break;
	}

	return a;
}

static unsigned
arenas_i2a(size_t i, unsigned narenas) {
	return arenas_i2a_impl(i, narenas, true, false);
}

static ctl_arena_t *
arenas_i_impl(tsd_t *tsd, size_t i, bool compat, bool init) {
	ctl_arena_t *ret;

	assert(!compat || !init);
	unsigned narenas = ctl_narenas_get(tsd_tsdn(tsd));

	ret = ctl_arenas->arenas[arenas_i2a_impl(i, narenas, compat, false)];
	if (init && ret == NULL) {
		if (config_stats) {
			struct container_s {
				ctl_arena_t       ctl_arena;
				ctl_arena_stats_t astats;
			};
			struct container_s *cont = (struct container_s *)
			    base_alloc(tsd_tsdn(tsd), b0get(),
			        sizeof(struct container_s), QUANTUM);
			if (cont == NULL) {
				return NULL;
			}
			ret = &cont->ctl_arena;
			ret->astats = &cont->astats;
		} else {
			ret = (ctl_arena_t *)base_alloc(tsd_tsdn(tsd), b0get(),
			    sizeof(ctl_arena_t), QUANTUM);
			if (ret == NULL) {
				return NULL;
			}
		}
		ret->arena_ind = (unsigned)i;
		ctl_arenas->arenas[arenas_i2a_impl(i, narenas, compat, false)]
		    = ret;
	}

	assert(ret == NULL ||
	    arenas_i2a(ret->arena_ind, narenas) == arenas_i2a(i, narenas));
	return ret;
}

ctl_arena_t *
ctl_arenas_i(size_t i) {
	ctl_arena_t *ret = arenas_i_impl(tsd_fetch(), i, true, false);
	assert(ret != NULL);
	return ret;
}

static void
ctl_arena_clear(ctl_arena_t *ctl_arena) {
	ctl_arena->nthreads = 0;
	ctl_arena->dss = dss_prec_names[dss_prec_limit];
	ctl_arena->dirty_decay_ms = -1;
	ctl_arena->muzzy_decay_ms = -1;
	ctl_arena->pactive = 0;
	ctl_arena->pdirty = 0;
	ctl_arena->pmuzzy = 0;
	if (config_stats) {
		memset(ctl_arena->astats, 0, sizeof(*(ctl_arena->astats)));
	}
}

static void
ctl_arena_stats_amerge(tsdn_t *tsdn, ctl_arena_t *ctl_arena, arena_t *arena) {
	unsigned i;

	if (config_stats) {
		arena_stats_merge(tsdn, arena, &ctl_arena->nthreads,
		    &ctl_arena->dss, &ctl_arena->dirty_decay_ms,
		    &ctl_arena->muzzy_decay_ms, &ctl_arena->pactive,
		    &ctl_arena->pdirty, &ctl_arena->pmuzzy,
		    &ctl_arena->astats->astats, ctl_arena->astats->bstats,
		    ctl_arena->astats->lstats, ctl_arena->astats->estats,
		    &ctl_arena->astats->hpastats);

		for (i = 0; i < SC_NBINS; i++) {
			bin_stats_t *bstats =
			    &ctl_arena->astats->bstats[i].stats_data;
			ctl_arena->astats->allocated_small += bstats->curregs
			    * sz_index2size(i);
			ctl_arena->astats->nmalloc_small += bstats->nmalloc;
			ctl_arena->astats->ndalloc_small += bstats->ndalloc;
			ctl_arena->astats->nrequests_small += bstats->nrequests;
			ctl_arena->astats->nfills_small += bstats->nfills;
			ctl_arena->astats->nflushes_small += bstats->nflushes;
		}
	} else {
		arena_basic_stats_merge(tsdn, arena, &ctl_arena->nthreads,
		    &ctl_arena->dss, &ctl_arena->dirty_decay_ms,
		    &ctl_arena->muzzy_decay_ms, &ctl_arena->pactive,
		    &ctl_arena->pdirty, &ctl_arena->pmuzzy);
	}
}

static void
ctl_arena_stats_sdmerge(
    ctl_arena_t *ctl_sdarena, ctl_arena_t *ctl_arena, bool destroyed) {
	unsigned i;

	if (!destroyed) {
		ctl_sdarena->nthreads += ctl_arena->nthreads;
		ctl_sdarena->pactive += ctl_arena->pactive;
		ctl_sdarena->pdirty += ctl_arena->pdirty;
		ctl_sdarena->pmuzzy += ctl_arena->pmuzzy;
	} else {
		assert(ctl_arena->nthreads == 0);
		assert(ctl_arena->pactive == 0);
		assert(ctl_arena->pdirty == 0);
		assert(ctl_arena->pmuzzy == 0);
	}

	if (config_stats) {
		ctl_arena_stats_t *sdstats = ctl_sdarena->astats;
		ctl_arena_stats_t *astats = ctl_arena->astats;

		if (!destroyed) {
			sdstats->astats.mapped += astats->astats.mapped;
			sdstats->astats.pa_shard_stats.pac_stats.retained +=
			    astats->astats.pa_shard_stats.pac_stats.retained;
			sdstats->astats.pa_shard_stats.pac_stats.pinned +=
			    astats->astats.pa_shard_stats.pac_stats.pinned;
			sdstats->astats.pa_shard_stats.edata_avail +=
			    astats->astats.pa_shard_stats.edata_avail;
		}

		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_dirty.npurge,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty
		        .npurge);
		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_dirty.nmadvise,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty
		        .nmadvise);
		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_dirty.purged,
		    &astats->astats.pa_shard_stats.pac_stats.decay_dirty
		        .purged);

		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_muzzy.npurge,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy
		        .npurge);
		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_muzzy.nmadvise,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy
		        .nmadvise);
		ctl_accum_locked_u64(&sdstats->astats.pa_shard_stats.pac_stats
		                         .decay_muzzy.purged,
		    &astats->astats.pa_shard_stats.pac_stats.decay_muzzy
		        .purged);

#define OP(mtx)                                                                \
	malloc_mutex_prof_merge(                                               \
	    &(sdstats->astats.mutex_prof_data[arena_prof_mutex_##mtx]),        \
	    &(astats->astats.mutex_prof_data[arena_prof_mutex_##mtx]));
		MUTEX_PROF_ARENA_MUTEXES
#undef OP
		if (!destroyed) {
			sdstats->astats.base += astats->astats.base;
			sdstats->astats.metadata_edata +=
			    astats->astats.metadata_edata;
			sdstats->astats.metadata_rtree +=
			    astats->astats.metadata_rtree;
			sdstats->astats.resident += astats->astats.resident;
			sdstats->astats.metadata_thp +=
			    astats->astats.metadata_thp;
			ctl_accum_atomic_zu(&sdstats->astats.internal,
			    &astats->astats.internal);
		} else {
			assert(atomic_load_zu(
			           &astats->astats.internal, ATOMIC_RELAXED)
			    == 0);
		}

		if (!destroyed) {
			sdstats->allocated_small += astats->allocated_small;
		} else {
			assert(astats->allocated_small == 0);
		}
		sdstats->nmalloc_small += astats->nmalloc_small;
		sdstats->ndalloc_small += astats->ndalloc_small;
		sdstats->nrequests_small += astats->nrequests_small;
		sdstats->nfills_small += astats->nfills_small;
		sdstats->nflushes_small += astats->nflushes_small;

		if (!destroyed) {
			sdstats->astats.allocated_large +=
			    astats->astats.allocated_large;
		} else {
			assert(astats->astats.allocated_large == 0);
		}
		sdstats->astats.nmalloc_large += astats->astats.nmalloc_large;
		sdstats->astats.ndalloc_large += astats->astats.ndalloc_large;
		sdstats->astats.nrequests_large +=
		    astats->astats.nrequests_large;
		sdstats->astats.nflushes_large += astats->astats.nflushes_large;
		ctl_accum_atomic_zu(
		    &sdstats->astats.pa_shard_stats.pac_stats.abandoned_vm,
		    &astats->astats.pa_shard_stats.pac_stats.abandoned_vm);

		sdstats->astats.tcache_bytes += astats->astats.tcache_bytes;
		sdstats->astats.tcache_stashed_bytes +=
		    astats->astats.tcache_stashed_bytes;

		if (ctl_arena->arena_ind == 0) {
			sdstats->astats.uptime = astats->astats.uptime;
		}

		for (i = 0; i < SC_NBINS; i++) {
			bin_stats_t *bstats = &astats->bstats[i].stats_data;
			bin_stats_t *merged = &sdstats->bstats[i].stats_data;
			merged->nmalloc += bstats->nmalloc;
			merged->ndalloc += bstats->ndalloc;
			merged->nrequests += bstats->nrequests;
			if (!destroyed) {
				merged->curregs += bstats->curregs;
			} else {
				assert(bstats->curregs == 0);
			}
			merged->nfills += bstats->nfills;
			merged->nflushes += bstats->nflushes;
			merged->nslabs += bstats->nslabs;
			merged->reslabs += bstats->reslabs;
			if (!destroyed) {
				merged->curslabs += bstats->curslabs;
				merged->nonfull_slabs += bstats->nonfull_slabs;
			} else {
				assert(bstats->curslabs == 0);
				assert(bstats->nonfull_slabs == 0);
			}
			malloc_mutex_prof_merge(&sdstats->bstats[i].mutex_data,
			    &astats->bstats[i].mutex_data);
		}

		for (i = 0; i < SC_NSIZES - SC_NBINS; i++) {
			ctl_accum_locked_u64(&sdstats->lstats[i].nmalloc,
			    &astats->lstats[i].nmalloc);
			ctl_accum_locked_u64(&sdstats->lstats[i].ndalloc,
			    &astats->lstats[i].ndalloc);
			ctl_accum_locked_u64(&sdstats->lstats[i].nrequests,
			    &astats->lstats[i].nrequests);
			if (!destroyed) {
				sdstats->lstats[i].curlextents +=
				    astats->lstats[i].curlextents;
			} else {
				assert(astats->lstats[i].curlextents == 0);
			}
		}

		for (i = 0; i < SC_NPSIZES; i++) {
			sdstats->estats[i].ndirty += astats->estats[i].ndirty;
			sdstats->estats[i].nmuzzy += astats->estats[i].nmuzzy;
			sdstats->estats[i].nretained +=
			    astats->estats[i].nretained;
			sdstats->estats[i].npinned +=
			    astats->estats[i].npinned;
			sdstats->estats[i].dirty_bytes +=
			    astats->estats[i].dirty_bytes;
			sdstats->estats[i].muzzy_bytes +=
			    astats->estats[i].muzzy_bytes;
			sdstats->estats[i].retained_bytes +=
			    astats->estats[i].retained_bytes;
			sdstats->estats[i].pinned_bytes +=
			    astats->estats[i].pinned_bytes;
		}

		hpa_shard_stats_accum(&sdstats->hpastats, &astats->hpastats);
	}
}

static void
ctl_arena_refresh_one(tsdn_t *tsdn, arena_t *arena, ctl_arena_t *ctl_sdarena,
    unsigned i, bool destroyed) {
	ctl_arena_t *ctl_arena = ctl_arenas_i(i);

	ctl_arena_clear(ctl_arena);
	ctl_arena_stats_amerge(tsdn, ctl_arena, arena);
	ctl_arena_stats_sdmerge(ctl_sdarena, ctl_arena, destroyed);
}

static unsigned
ctl_arena_init(tsd_t *tsd, const arena_config_t *config) {
	unsigned     arena_ind;
	ctl_arena_t *ctl_arena;

	ctl_mtx_assert_held(tsd_tsdn(tsd));

	if ((ctl_arena = ql_last(&ctl_arenas->destroyed, destroyed_link))
	    != NULL) {
		ql_remove(&ctl_arenas->destroyed, ctl_arena, destroyed_link);
		arena_ind = ctl_arena->arena_ind;
	} else {
		arena_ind = ctl_arenas->narenas;
	}

	if (arenas_i_impl(tsd, arena_ind, false, true) == NULL) {
		return UINT_MAX;
	}

	if (arena_init(tsd_tsdn(tsd), arena_ind, config) == NULL) {
		return UINT_MAX;
	}

	if (arena_ind == ctl_arenas->narenas) {
		ctl_arenas->narenas++;
	}

	return arena_ind;
}

bool
ctl_arenas_init(tsd_t *tsd) {
	tsdn_t *tsdn = tsd_tsdn(tsd);
	ctl_mtx_assert_held(tsdn);

	if (ctl_arenas == NULL) {
		ctl_arenas = (ctl_arenas_t *)base_alloc(
		    tsdn, b0get(), sizeof(ctl_arenas_t), QUANTUM);
		if (ctl_arenas == NULL) {
			return true;
		}
	}

	ctl_arena_t *ctl_sarena, *ctl_darena;
	if ((ctl_sarena = arenas_i_impl(tsd, MALLCTL_ARENAS_ALL, false, true))
	    == NULL) {
		return true;
	}
	ctl_sarena->initialized = true;

	if ((ctl_darena = arenas_i_impl(
	         tsd, MALLCTL_ARENAS_DESTROYED, false, true))
	    == NULL) {
		return true;
	}
	ctl_arena_clear(ctl_darena);

	ctl_arenas->narenas = narenas_total_get();
	for (unsigned i = 0; i < ctl_arenas->narenas; i++) {
		if (arenas_i_impl(tsd, i, false, true) == NULL) {
			return true;
		}
	}

	ql_new(&ctl_arenas->destroyed);
	return false;
}

ctl_arena_t *
ctl_arenas_refresh(tsdn_t *tsdn) {
	ctl_mtx_assert_held(tsdn);

	const unsigned narenas = ctl_arenas->narenas;
	assert(narenas > 0);
	ctl_arena_t *ctl_sarena = ctl_arenas_i(MALLCTL_ARENAS_ALL);
	VARIABLE_ARRAY_UNSAFE(arena_t *, tarenas, narenas);

	ctl_arena_clear(ctl_sarena);

	for (unsigned i = 0; i < narenas; i++) {
		tarenas[i] = arena_get(tsdn, i, false);
	}

	for (unsigned i = 0; i < narenas; i++) {
		ctl_arena_t *ctl_arena = ctl_arenas_i(i);
		bool         initialized = (tarenas[i] != NULL);

		ctl_arena->initialized = initialized;
		if (initialized) {
			ctl_arena_refresh_one(
			    tsdn, tarenas[i], ctl_sarena, i, false);
		}
	}

	return ctl_sarena;
}

uint64_t
ctl_arenas_epoch_get(void) {
	return ctl_arenas->epoch;
}

void
ctl_arenas_epoch_advance(void) {
	ctl_arenas->epoch++;
}

bool
ctl_arena_i_indexable(tsdn_t *tsdn, size_t i) {
	bool ret;

	ctl_mtx_lock(tsdn);
	switch (i) {
	case MALLCTL_ARENAS_ALL:
	case MALLCTL_ARENAS_DESTROYED:
		ret = true;
		break;
	default:
		ret = (i <= ctl_narenas_get(tsdn));
		break;
	}
	ctl_mtx_unlock(tsdn);
	return ret;
}

bool
ctl_arenas_i_verify(size_t i, unsigned narenas) {
	size_t a = arenas_i2a_impl(i, narenas, true, true);
	if (a == UINT_MAX || !ctl_arenas->arenas[a]->initialized) {
		return true;
	}

	return false;
}

int
ctl_arena_create(tsd_t *tsd, void *oldp, size_t *oldlenp,
    const arena_config_t *config) {
	unsigned arena_ind = ctl_arena_init(tsd, config);
	if (arena_ind == UINT_MAX) {
		return EAGAIN;
	}
	return ctl_read(oldp, oldlenp, &arena_ind, sizeof(arena_ind));
}

/******************************************************************************/
/* arena.<i> mallctl handlers. */

int
arena_i_initialized_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	tsdn_t  *tsdn = tsd_tsdn(tsd);
	unsigned arena_ind = 0;
	bool     initialized;

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	}
	if (ret == 0) {
		ctl_mtx_lock(tsdn);
		initialized = ctl_arenas_i(arena_ind)->initialized;
		ctl_mtx_unlock(tsdn);

		ret = ctl_read(oldp, oldlenp, &initialized, sizeof(initialized));
	}
	return ret;
}

static void
arena_i_decay(tsdn_t *tsdn, unsigned arena_ind, bool all) {
	ctl_mtx_lock(tsdn);
	unsigned narenas = ctl_narenas_get(tsdn);

	/*
	 * Access via index narenas is deprecated, and scheduled for
	 * removal in 6.0.0.
	 */
	bool decay_all = ctl_arena_ind_is_all(arena_ind, narenas);
	unsigned count = decay_all ? narenas : 1;
	VARIABLE_ARRAY_UNSAFE(arena_t *, tarenas, count);

	if (decay_all) {
		for (unsigned i = 0; i < narenas; i++) {
			tarenas[i] = arena_get(tsdn, i, false);
		}
	} else {
		assert(arena_ind < narenas);
		tarenas[0] = arena_get(tsdn, arena_ind, false);
	}
	ctl_mtx_unlock(tsdn);

	for (unsigned i = 0; i < count; i++) {
		if (tarenas[i] != NULL) {
			arena_decay(tsdn, tarenas[i], false, all);
		}
	}
}

int
arena_i_decay_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind = 0;

	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	}
	if (ret == 0) {
		arena_i_decay(tsd_tsdn(tsd), arena_ind, false);
	}
	return ret;
}

int
arena_i_purge_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind = 0;

	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	}
	if (ret == 0) {
		arena_i_decay(tsd_tsdn(tsd), arena_ind, true);
	}
	return ret;
}

static int
arena_i_reset_destroy_helper(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen, unsigned *arena_ind,
    arena_t **arena) {
	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(arena_ind, mib, 1);
	}
	if (ret == 0) {
		*arena = arena_get(tsd_tsdn(tsd), *arena_ind, false);
		if (*arena == NULL || arena_is_auto(*arena)) {
			ret = EFAULT;
		}
	}
	return ret;
}

static void
arena_reset_prepare_background_thread(tsd_t *tsd, unsigned arena_ind) {
	if (have_background_thread) {
		malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);
		if (background_thread_enabled()) {
			background_thread_info_t *info =
			    background_thread_info_get(arena_ind);
			assert(info->state == background_thread_started);
			malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
			info->state = background_thread_paused;
			malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		}
	}
}

static void
arena_reset_finish_background_thread(tsd_t *tsd, unsigned arena_ind) {
	if (have_background_thread) {
		if (background_thread_enabled()) {
			background_thread_info_t *info =
			    background_thread_info_get(arena_ind);
			assert(info->state == background_thread_paused);
			malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
			info->state = background_thread_started;
			malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	}
}

int
arena_i_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int      ret;
	unsigned arena_ind;
	arena_t *arena;

	ret = arena_i_reset_destroy_helper(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, &arena_ind, &arena);
	if (ret != 0) {
		return ret;
	}

	arena_reset_prepare_background_thread(tsd, arena_ind);
	arena_reset(tsd, arena);
	arena_reset_finish_background_thread(tsd, arena_ind);

	return ret;
}

int
arena_i_destroy_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int          ret;
	unsigned     arena_ind;
	arena_t     *arena;
	ctl_arena_t *ctl_darena, *ctl_arena;

	ctl_mtx_lock(tsd_tsdn(tsd));

	ret = arena_i_reset_destroy_helper(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, &arena_ind, &arena);
	if (ret != 0) {
		goto label_return;
	}

	if (arena_nthreads_get(arena, false) != 0
	    || arena_nthreads_get(arena, true) != 0) {
		ret = EFAULT;
		goto label_return;
	}

	arena_reset_prepare_background_thread(tsd, arena_ind);
	arena_reset(tsd, arena);
	arena_decay(tsd_tsdn(tsd), arena, false, true);
	ctl_darena = ctl_arenas_i(MALLCTL_ARENAS_DESTROYED);
	ctl_darena->initialized = true;
	ctl_arena_refresh_one(
	    tsd_tsdn(tsd), arena, ctl_darena, arena_ind, true);
	arena_destroy(tsd, arena);
	ctl_arena = ctl_arenas_i(arena_ind);
	ctl_arena->initialized = false;
	ql_elm_new(ctl_arena, destroyed_link);
	ql_tail_insert(&ctl_arenas->destroyed, ctl_arena, destroyed_link);
	arena_reset_finish_background_thread(tsd, arena_ind);

	assert(ret == 0);
label_return:
	ctl_mtx_unlock(tsd_tsdn(tsd));

	return ret;
}

int
arena_i_dss_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	const char *dss = NULL;
	unsigned    arena_ind = 0;
	dss_prec_t  dss_prec = dss_prec_limit;

	ctl_mtx_lock(tsd_tsdn(tsd));
	int ret = ctl_write(&dss, sizeof(dss), newp, newlen);
	if (ret != 0) {
		goto label_return;
	}
	ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	if (ret != 0) {
		goto label_return;
	}

	if (dss != NULL) {
		int  i;
		bool match = false;

		for (i = 0; i < dss_prec_limit; i++) {
			if (strcmp(dss_prec_names[i], dss) == 0) {
				dss_prec = i;
				match = true;
				break;
			}
		}

		if (!match) {
			ret = EINVAL;
			goto label_return;
		}
	}

	/*
	 * Access via index narenas is deprecated, and scheduled for removal in
	 * 6.0.0.
	 */
	dss_prec_t dss_prec_old = dss_prec_limit;
	unsigned narenas = ctl_narenas_get(tsd_tsdn(tsd));
	if (ctl_arena_ind_is_all(arena_ind, narenas)) {
		if (dss_prec != dss_prec_limit
		    && extent_dss_prec_set(dss_prec)) {
			ret = EFAULT;
			goto label_return;
		}
		dss_prec_old = extent_dss_prec_get();
	} else {
		arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
		if (arena == NULL
		    || (dss_prec != dss_prec_limit
		        && arena_dss_prec_set(arena, dss_prec))) {
			ret = EFAULT;
			goto label_return;
		}
		dss_prec_old = arena_dss_prec_get(arena);
	}

	dss = dss_prec_names[dss_prec_old];
	ret = ctl_read(oldp, oldlenp, &dss, sizeof(dss));
label_return:
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
arena_i_oversize_threshold_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind;
	int ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	if (ret != 0) {
		return ret;
	}

	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	if (arena == NULL) {
		return EFAULT;
	}

	size_t oldval = atomic_load_zu(
	    &arena->pa_shard.pac.oversize_threshold, ATOMIC_RELAXED);
	ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret != 0 || newp == NULL) {
		return ret;
	}

	size_t newval;
	ret = ctl_write(&newval, sizeof(newval), newp, newlen);
	if (ret == 0) {
		atomic_store_zu(&arena->pa_shard.pac.oversize_threshold, newval,
		    ATOMIC_RELAXED);
	}
	return ret;
}

static int
arena_i_decay_ms_ctl_impl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen, bool dirty) {
	unsigned arena_ind;
	int ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	if (ret != 0) {
		return ret;
	}

	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	if (arena == NULL) {
		return EFAULT;
	}

	extent_state_t state = dirty ? extent_state_dirty : extent_state_muzzy;
	ssize_t oldval = arena_decay_ms_get(arena, state);
	ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret != 0 || newp == NULL) {
		return ret;
	}

	ssize_t newval;
	ret = ctl_write(&newval, sizeof(newval), newp, newlen);
	if (ret == 0 && arena_decay_ms_set(tsd_tsdn(tsd), arena, state, newval)) {
		ret = EFAULT;
	}
	return ret;
}

int
arena_i_dirty_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arena_i_decay_ms_ctl_impl(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, true);
}

int
arena_i_muzzy_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arena_i_decay_ms_ctl_impl(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, false);
}

int
arena_i_extent_hooks_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind;
	arena_t *arena;

	ctl_mtx_lock(tsd_tsdn(tsd));
	int ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	if (ret == 0 && arena_ind >= narenas_total_get()) {
		ret = EFAULT;
	}
	if (ret == 0) {
		extent_hooks_t *old_extent_hooks;
		arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
		if (arena == NULL) {
			if (arena_ind >= narenas_auto) {
				ret = EFAULT;
			} else {
				old_extent_hooks =
				    (extent_hooks_t *)&ehooks_default_extent_hooks;
				ret = ctl_read(oldp, oldlenp, &old_extent_hooks,
				    sizeof(extent_hooks_t *));
			}
			if (ret == 0 && newp != NULL) {
				/* Initialize a new arena as a side effect. */
				extent_hooks_t *new_extent_hooks
				    JEMALLOC_CC_SILENCE_INIT(NULL);
				ret = ctl_write(&new_extent_hooks,
				    sizeof(extent_hooks_t *), newp, newlen);
				if (ret == 0) {
					arena_config_t config = arena_config_default;
					config.extent_hooks = new_extent_hooks;

					arena = arena_init(
					    tsd_tsdn(tsd), arena_ind, &config);
					if (arena == NULL) {
						ret = EFAULT;
					}
				}
			}
		} else {
			if (newp != NULL) {
				extent_hooks_t *new_extent_hooks
				    JEMALLOC_CC_SILENCE_INIT(NULL);
				ret = ctl_write(&new_extent_hooks,
				    sizeof(extent_hooks_t *), newp, newlen);
				if (ret == 0) {
					old_extent_hooks = arena_set_extent_hooks(
					    tsd, arena, new_extent_hooks);
					ret = ctl_read(oldp, oldlenp,
					    &old_extent_hooks,
					    sizeof(extent_hooks_t *));
				}
			} else {
				old_extent_hooks = ehooks_get_extent_hooks_ptr(
				    arena_get_ehooks(arena));
				ret = ctl_read(oldp, oldlenp, &old_extent_hooks,
				    sizeof(extent_hooks_t *));
			}
		}
	}
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
arena_i_retain_grow_limit_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind;

	if (!opt_retain) {
		/* Only relevant when retain is enabled. */
		return ENOENT;
	}

	ctl_mtx_lock(tsd_tsdn(tsd));

	int ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	arena_t *arena = NULL;
	if (ret == 0) {
		arena = arena_ind < narenas_total_get() ?
		    arena_get(tsd_tsdn(tsd), arena_ind, false) : NULL;
		if (arena == NULL) {
			ret = EFAULT;
		}
	}

	size_t old_limit;
	size_t new_limit;
	if (ret == 0) {
		ret = ctl_write(&new_limit, sizeof(new_limit), newp, newlen);
	}
	if (ret == 0) {
		bool err = arena_retain_grow_limit_get_set(
		    tsd, arena, &old_limit, newp != NULL ? &new_limit : NULL);
		ret = err ? EFAULT : 0;
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &old_limit, sizeof(old_limit));
	}

	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
arena_i_name_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned arena_ind;

	ctl_mtx_lock(tsd_tsdn(tsd));

	int ret = ctl_mib_unsigned(&arena_ind, mib, 1);
	if (ret != 0) {
		goto label_return;
	}
	unsigned narenas = ctl_narenas_get(tsd_tsdn(tsd));
	if (ctl_arena_ind_is_all(arena_ind, narenas) || arena_ind > narenas) {
		ret = EINVAL;
		goto label_return;
	}

	arena_t *arena = arena_get(tsd_tsdn(tsd), arena_ind, false);
	if (arena == NULL) {
		ret = EFAULT;
		goto label_return;
	}

	if (oldp != NULL && oldlenp != NULL) {
		/*
		 * Read the arena name.  When reading, the input oldp should
		 * point to an array with a length no shorter than
		 * ARENA_NAME_LEN or the length when it was set.
		 */
		if (*oldlenp != sizeof(char *)) {
			ret = EINVAL;
			goto label_return;
		}
		char *old_name = *(char **)oldp;
		arena_name_get(arena, old_name);
	}

	char *new_name = NULL;
	ret = ctl_write(&new_name, sizeof(new_name), newp, newlen);
	if (ret != 0) {
		goto label_return;
	}
	if (newp != NULL) {
		if (new_name == NULL) {
			ret = EINVAL;
			goto label_return;
		}
		/* Write the arena name. */
		arena_name_set(arena, new_name);
	}

label_return:
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

/******************************************************************************/
/* arenas.* mallctl handlers. */

int
arenas_narenas_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	ctl_mtx_lock(tsd_tsdn(tsd));
	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		unsigned narenas = ctl_narenas_get(tsd_tsdn(tsd));
		ret = ctl_read(oldp, oldlenp, &narenas, sizeof(narenas));
	}
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

static int
arenas_decay_ms_ctl_impl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen, bool dirty) {
	ssize_t oldval = dirty ? arena_dirty_decay_ms_default_get()
	                       : arena_muzzy_decay_ms_default_get();
	int ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret == 0 && newp != NULL) {
		ssize_t newval;
		ret = ctl_write(&newval, sizeof(newval), newp, newlen);
		if (ret == 0
		    && (dirty ? arena_dirty_decay_ms_default_set(newval)
		              : arena_muzzy_decay_ms_default_set(newval))) {
			ret = EFAULT;
		}
	}
	return ret;
}

int
arenas_dirty_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arenas_decay_ms_ctl_impl(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, true);
}

int
arenas_muzzy_decay_ms_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	return arenas_decay_ms_ctl_impl(
	    tsd, mib, miblen, oldp, oldlenp, newp, newlen, false);
}

CTL_RO_NL_GEN_PUBLIC(arenas_quantum, QUANTUM, size_t)
CTL_RO_NL_GEN_PUBLIC(arenas_page, PAGE, size_t)
CTL_RO_NL_GEN_PUBLIC(arenas_hugepage, HUGEPAGE, size_t)
CTL_RO_NL_GEN_PUBLIC(
    arenas_tcache_max, global_do_not_change_tcache_maxclass, size_t)
CTL_RO_NL_GEN_PUBLIC(arenas_nbins, SC_NBINS, unsigned)
CTL_RO_NL_GEN_PUBLIC(arenas_nhbins, global_do_not_change_tcache_nbins, unsigned)
CTL_RO_NL_GEN_PUBLIC(arenas_bin_i_size, bin_infos[mib[2]].reg_size, size_t)
CTL_RO_NL_GEN_PUBLIC(arenas_bin_i_nregs, bin_infos[mib[2]].nregs, uint32_t)
CTL_RO_NL_GEN_PUBLIC(
    arenas_bin_i_slab_size, bin_infos[mib[2]].slab_size, size_t)
CTL_RO_NL_GEN_PUBLIC(
    arenas_bin_i_nshards, bin_infos[mib[2]].n_shards, uint32_t)
CTL_RO_NL_GEN_PUBLIC(arenas_nlextents, SC_NSIZES - SC_NBINS, unsigned)
CTL_RO_NL_GEN_PUBLIC(arenas_lextent_i_size,
    sz_index2size_unsafe(SC_NBINS + (szind_t)mib[2]), size_t)

int
arenas_create_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	ctl_mtx_lock(tsd_tsdn(tsd));

	int ret = ctl_verify_read(oldp, oldlenp, sizeof(unsigned));
	arena_config_t config = arena_config_default;
	if (ret == 0) {
		ret = ctl_write(&config.extent_hooks,
		    sizeof(extent_hooks_t *), newp, newlen);
	}

	if (ret == 0) {
		ret = ctl_arena_create(tsd, oldp, oldlenp, &config);
	}
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
experimental_arenas_create_ext_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	ctl_mtx_lock(tsd_tsdn(tsd));

	arena_config_t config = arena_config_default;
	int ret = ctl_verify_read(oldp, oldlenp, sizeof(unsigned));
	if (ret == 0) {
		ret = ctl_write(&config, sizeof(config), newp, newlen);
	}

	if (ret == 0) {
		ret = ctl_arena_create(tsd, oldp, oldlenp, &config);
	}
	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
arenas_lookup_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	void *ptr = NULL;

	ctl_mtx_lock(tsd_tsdn(tsd));

	int ret = ctl_write(&ptr, sizeof(ptr), newp, newlen);
	emap_full_alloc_ctx_t alloc_ctx;
	if (ret == 0) {
		bool ptr_not_present = emap_full_alloc_ctx_try_lookup(
		    tsd_tsdn(tsd), &arena_emap_global, ptr, &alloc_ctx);
		if (ptr_not_present || alloc_ctx.edata == NULL) {
			ret = EINVAL;
		}
	}

	arena_t *arena = NULL;
	if (ret == 0) {
		arena = arena_get_from_edata(alloc_ctx.edata);
		if (arena == NULL) {
			ret = EINVAL;
		}
	}

	if (ret == 0) {
		unsigned arena_ind = arena_ind_get(arena);
		ret = ctl_read(oldp, oldlenp, &arena_ind, sizeof(arena_ind));
	}

	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}
