#define	JEMALLOC_TCACHE_C_
#include "internal/jemalloc_internal.h"
#ifdef JEMALLOC_TCACHE
/******************************************************************************/
/* Data. */

size_t	opt_lg_tcache_nslots = LG_TCACHE_NSLOTS_DEFAULT;
ssize_t	opt_lg_tcache_gc_sweep = LG_TCACHE_GC_SWEEP_DEFAULT;
bool	opt_tcache_sort = true;

/* Map of thread-specific caches. */
__thread tcache_t	*tcache_tls JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Same contents as tcache, but initialized such that the TSD destructor is
 * called when a thread exits, so that the cache can be cleaned up.
 */
static pthread_key_t		tcache_tsd;

size_t				tcache_nslots;
unsigned			tcache_gc_incr;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	tcache_thread_cleanup(void *arg);

/******************************************************************************/

void *
tcache_alloc_hard(tcache_t *tcache, tcache_bin_t *tbin, size_t binind)
{
	void *ret;

	arena_tcache_fill(tcache->arena, tbin, binind);
	ret = tcache_bin_alloc(tbin);

	return (ret);
}

static inline void
tcache_bin_merge(void **to, void **fr, unsigned lcnt, unsigned rcnt)
{
	void **l, **r;
	unsigned li, ri, i;

	l = fr;
	r = &fr[lcnt];
	li = ri = i = 0;
	while (li < lcnt && ri < rcnt) {
		/* High pointers come first in sorted result. */
		if ((uintptr_t)l[li] > (uintptr_t)r[ri]) {
			to[i] = l[li];
			li++;
		} else {
			to[i] = r[ri];
			ri++;
		}
		i++;
	}

	if (li < lcnt)
		memcpy(&to[i], &l[li], sizeof(void *) * (lcnt - li));
	else if (ri < rcnt)
		memcpy(&to[i], &r[ri], sizeof(void *) * (rcnt - ri));
}

static inline void
tcache_bin_sort(tcache_bin_t *tbin)
{
	unsigned e, i;
	void **fr, **to;
	void *mslots[tcache_nslots];

	/*
	 * Perform iterative merge sort, swapping source and destination arrays
	 * during each iteration.
	 */

	fr = mslots; to = tbin->slots;
	for (e = 1; e < tbin->ncached; e <<= 1) {
		void **tmp = fr; fr = to; to = tmp;
		for (i = 0; i + (e << 1) <= tbin->ncached; i += (e << 1))
			tcache_bin_merge(&to[i], &fr[i], e, e);
		if (i + e <= tbin->ncached) {
			tcache_bin_merge(&to[i], &fr[i],
			    e, tbin->ncached - (i + e));
		} else if (i < tbin->ncached)
			tcache_bin_merge(&to[i], &fr[i], tbin->ncached - i, 0);
	}

	/* Copy the final result out of mslots, if necessary. */
	if (to == mslots)
		memcpy(tbin->slots, mslots, sizeof(void *) * tbin->ncached);

#ifdef JEMALLOC_DEBUG
	for (i = 1; i < tbin->ncached; i++)
		assert(tbin->slots[i-1] > tbin->slots[i]);
#endif
}

void
tcache_bin_flush(tcache_bin_t *tbin, size_t binind, unsigned rem)
{
	arena_chunk_t *chunk;
	arena_t *arena;
	void *ptr;
	unsigned i, ndeferred, ncached;

	if (opt_tcache_sort && rem > 0) {
		assert(rem < tbin->ncached);
		/* Sort pointers such that the highest objects will be freed. */
		tcache_bin_sort(tbin);
	}

	for (ndeferred = tbin->ncached - rem; ndeferred > 0;) {
		ncached = ndeferred;
		/* Lock the arena associated with the first object. */
		chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(tbin->slots[0]);
		arena = chunk->arena;
		malloc_mutex_lock(&arena->lock);
		/* Deallocate every object that belongs to the locked arena. */
		for (i = ndeferred = 0; i < ncached; i++) {
			ptr = tbin->slots[i];
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
			if (chunk->arena == arena) {
				size_t pageind = (((uintptr_t)ptr -
				    (uintptr_t)chunk) >> PAGE_SHIFT);
				arena_chunk_map_t *mapelm =
				    &chunk->map[pageind];
				arena_dalloc_bin(arena, chunk, ptr, mapelm);
			} else {
				/*
				 * This object was allocated via a different
				 * arena than the one that is currently locked.
				 * Stash the object, so that it can be handled
				 * in a future pass.
				 */
				tbin->slots[ndeferred] = ptr;
				ndeferred++;
			}
		}
#ifdef JEMALLOC_STATS
		arena->bins[binind].stats.nflushes++;
		{
			arena_bin_t *bin = &arena->bins[binind];
			bin->stats.nrequests += tbin->tstats.nrequests;
			if (bin->reg_size <= small_maxclass) {
				arena->stats.nmalloc_small +=
				    tbin->tstats.nrequests;
			} else {
				arena->stats.nmalloc_medium +=
				    tbin->tstats.nrequests;
			}
			tbin->tstats.nrequests = 0;
		}
#endif
		malloc_mutex_unlock(&arena->lock);
	}

	if (rem > 0) {
		/*
		 * Shift the remaining valid pointers to the base of the slots
		 * array.
		 */
		memmove(&tbin->slots[0], &tbin->slots[tbin->ncached - rem],
		    rem * sizeof(void *));
	}
	tbin->ncached = rem;
}

tcache_bin_t *
tcache_bin_create(arena_t *arena)
{
	tcache_bin_t *ret;
	size_t tsize;

	tsize = sizeof(tcache_bin_t) + (sizeof(void *) * (tcache_nslots - 1));
	if (tsize <= small_maxclass)
		ret = (tcache_bin_t *)arena_malloc_small(arena, tsize, false);
	else if (tsize <= bin_maxclass)
		ret = (tcache_bin_t *)arena_malloc_medium(arena, tsize, false);
	else
		ret = (tcache_bin_t *)imalloc(tsize);
	if (ret == NULL)
		return (NULL);
#ifdef JEMALLOC_STATS
	memset(&ret->tstats, 0, sizeof(tcache_bin_stats_t));
#endif
	ret->low_water = 0;
	ret->high_water = 0;
	ret->ncached = 0;

	return (ret);
}

void
tcache_bin_destroy(tcache_t *tcache, tcache_bin_t *tbin, unsigned binind)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	size_t pageind, tsize;
	arena_chunk_map_t *mapelm;

	chunk = CHUNK_ADDR2BASE(tbin);
	arena = chunk->arena;
	pageind = (((uintptr_t)tbin - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];

#ifdef JEMALLOC_STATS
	if (tbin->tstats.nrequests != 0) {
		arena_t *arena = tcache->arena;
		arena_bin_t *bin = &arena->bins[binind];
		malloc_mutex_lock(&arena->lock);
		bin->stats.nrequests += tbin->tstats.nrequests;
		if (bin->reg_size <= small_maxclass)
			arena->stats.nmalloc_small += tbin->tstats.nrequests;
		else
			arena->stats.nmalloc_medium += tbin->tstats.nrequests;
		malloc_mutex_unlock(&arena->lock);
	}
#endif

	assert(tbin->ncached == 0);
	tsize = sizeof(tcache_bin_t) + (sizeof(void *) * (tcache_nslots - 1));
	if (tsize <= bin_maxclass) {
		malloc_mutex_lock(&arena->lock);
		arena_dalloc_bin(arena, chunk, tbin, mapelm);
		malloc_mutex_unlock(&arena->lock);
	} else
		idalloc(tbin);
}

tcache_t *
tcache_create(arena_t *arena)
{
	tcache_t *tcache;

	if (sizeof(tcache_t) + (sizeof(tcache_bin_t *) * (nbins - 1)) <=
	    small_maxclass) {
		tcache = (tcache_t *)arena_malloc_small(arena, sizeof(tcache_t)
		    + (sizeof(tcache_bin_t *) * (nbins - 1)), true);
	} else if (sizeof(tcache_t) + (sizeof(tcache_bin_t *) * (nbins - 1)) <=
	    bin_maxclass) {
		tcache = (tcache_t *)arena_malloc_medium(arena, sizeof(tcache_t)
		    + (sizeof(tcache_bin_t *) * (nbins - 1)), true);
	} else {
		tcache = (tcache_t *)icalloc(sizeof(tcache_t) +
		    (sizeof(tcache_bin_t *) * (nbins - 1)));
	}

	if (tcache == NULL)
		return (NULL);

#ifdef JEMALLOC_STATS
	/* Link into list of extant tcaches. */
	malloc_mutex_lock(&arena->lock);
	ql_elm_new(tcache, link);
	ql_tail_insert(&arena->tcache_ql, tcache, link);
	malloc_mutex_unlock(&arena->lock);
#endif

	tcache->arena = arena;

	tcache_tls = tcache;
	pthread_setspecific(tcache_tsd, tcache);

	return (tcache);
}

void
tcache_destroy(tcache_t *tcache)
{
	unsigned i;

#ifdef JEMALLOC_STATS
	/* Unlink from list of extant tcaches. */
	malloc_mutex_lock(&tcache->arena->lock);
	ql_remove(&tcache->arena->tcache_ql, tcache, link);
	tcache_stats_merge(tcache, tcache->arena);
	malloc_mutex_unlock(&tcache->arena->lock);
#endif

	for (i = 0; i < nbins; i++) {
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			tcache_bin_flush(tbin, i, 0);
			tcache_bin_destroy(tcache, tbin, i);
		}
	}

	if (arena_salloc(tcache) <= bin_maxclass) {
		arena_chunk_t *chunk = CHUNK_ADDR2BASE(tcache);
		arena_t *arena = chunk->arena;
		size_t pageind = (((uintptr_t)tcache - (uintptr_t)chunk) >>
		    PAGE_SHIFT);
		arena_chunk_map_t *mapelm = &chunk->map[pageind];

		malloc_mutex_lock(&arena->lock);
		arena_dalloc_bin(arena, chunk, tcache, mapelm);
		malloc_mutex_unlock(&arena->lock);
	} else
		idalloc(tcache);
}

static void
tcache_thread_cleanup(void *arg)
{
	tcache_t *tcache = (tcache_t *)arg;

	assert(tcache == tcache_tls);
	if (tcache != NULL) {
		assert(tcache != (void *)(uintptr_t)1);
		tcache_destroy(tcache);
		tcache_tls = (void *)(uintptr_t)1;
	}
}

#ifdef JEMALLOC_STATS
void
tcache_stats_merge(tcache_t *tcache, arena_t *arena)
{
	unsigned i;

	/* Merge and reset tcache stats. */
	for (i = 0; i < mbin0; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			bin->stats.nrequests += tbin->tstats.nrequests;
			arena->stats.nmalloc_small += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
	}
	for (; i < nbins; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = tcache->tbins[i];
		if (tbin != NULL) {
			bin->stats.nrequests += tbin->tstats.nrequests;
			arena->stats.nmalloc_medium += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
	}
}
#endif

void
tcache_boot(void)
{

	if (opt_lg_tcache_nslots > 0) {
		tcache_nslots = (1U << opt_lg_tcache_nslots);

		/* Compute incremental GC event threshold. */
		if (opt_lg_tcache_gc_sweep >= 0) {
			tcache_gc_incr = ((1U << opt_lg_tcache_gc_sweep) /
			    nbins) + (((1U << opt_lg_tcache_gc_sweep) % nbins ==
			    0) ? 0 : 1);
		} else
			tcache_gc_incr = 0;
	} else
		tcache_nslots = 0;

	if (tcache_nslots != 0) {
		if (pthread_key_create(&tcache_tsd, tcache_thread_cleanup) !=
		    0) {
			malloc_write4("<jemalloc>",
			    ": Error in pthread_key_create()\n", "", "");
			abort();
		}
	}
}
/******************************************************************************/
#endif /* JEMALLOC_TCACHE */
