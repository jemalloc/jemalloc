#define	JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_internal.h"
#ifdef JEMALLOC_TCACHE
/******************************************************************************/
/* Data. */

bool	opt_tcache = true;
ssize_t	opt_lg_tcache_maxclass = LG_TCACHE_MAXCLASS_DEFAULT;
ssize_t	opt_lg_tcache_gc_sweep = LG_TCACHE_GC_SWEEP_DEFAULT;

/* Map of thread-specific caches. */
__thread tcache_t	*tcache_tls JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Same contents as tcache, but initialized such that the TSD destructor is
 * called when a thread exits, so that the cache can be cleaned up.
 */
static pthread_key_t		tcache_tsd;

size_t				nhbins;
size_t				tcache_maxclass;
unsigned			tcache_gc_incr;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	tcache_thread_cleanup(void *arg);

/******************************************************************************/

void *
tcache_alloc_small_hard(tcache_t *tcache, tcache_bin_t *tbin, size_t binind)
{
	void *ret;

	arena_tcache_fill_small(tcache->arena, tbin, binind
#ifdef JEMALLOC_PROF
	    , tcache->prof_accumbytes
#endif
	    );
#ifdef JEMALLOC_PROF
	tcache->prof_accumbytes = 0;
#endif
	ret = tcache_alloc_easy(tbin);

	return (ret);
}

void
tcache_bin_flush_small(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    )
{
	void *flush, *deferred, *ptr;
	unsigned i, nflush, ndeferred;

	assert(binind < nbins);
	assert(rem <= tbin->ncached);

	for (flush = tbin->avail, nflush = tbin->ncached - rem; flush != NULL;
	    flush = deferred, nflush = ndeferred) {
		/* Lock the arena bin associated with the first object. */
		arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(flush);
		arena_t *arena = chunk->arena;
		arena_bin_t *bin = &arena->bins[binind];

#ifdef JEMALLOC_PROF
		if (arena == tcache->arena) {
			malloc_mutex_lock(&arena->lock);
			arena_prof_accum(arena, tcache->prof_accumbytes);
			malloc_mutex_unlock(&arena->lock);
			tcache->prof_accumbytes = 0;
		}
#endif

		malloc_mutex_lock(&bin->lock);
#ifdef JEMALLOC_STATS
		if (arena == tcache->arena) {
			bin->stats.nflushes++;
			bin->stats.nrequests += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
#endif
		deferred = NULL;
		ndeferred = 0;
		for (i = 0; i < nflush; i++) {
			ptr = flush;
			assert(ptr != NULL);
			flush = *(void **)ptr;
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
				 * arena bin than the one that is currently
				 * locked.  Stash the object, so that it can be
				 * handled in a future pass.
				 */
				*(void **)ptr = deferred;
				deferred = ptr;
				ndeferred++;
			}
		}
		malloc_mutex_unlock(&bin->lock);

		if (flush != NULL) {
			/*
			 * This was the first pass, and rem cached objects
			 * remain.
			 */
			tbin->avail = flush;
		}
	}

	tbin->ncached = rem;
	if (tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
}

void
tcache_bin_flush_large(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    )
{
	void *flush, *deferred, *ptr;
	unsigned i, nflush, ndeferred;

	assert(binind < nhbins);
	assert(rem <= tbin->ncached);

	for (flush = tbin->avail, nflush = tbin->ncached - rem; flush != NULL;
	    flush = deferred, nflush = ndeferred) {
		/* Lock the arena associated with the first object. */
		arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(flush);
		arena_t *arena = chunk->arena;

		malloc_mutex_lock(&arena->lock);
#if (defined(JEMALLOC_PROF) || defined(JEMALLOC_STATS))
		if (arena == tcache->arena) {
#endif
#ifdef JEMALLOC_PROF
			arena_prof_accum(arena, tcache->prof_accumbytes);
			tcache->prof_accumbytes = 0;
#endif
#ifdef JEMALLOC_STATS
			arena->stats.nrequests_large += tbin->tstats.nrequests;
			arena->stats.lstats[binind - nbins].nrequests +=
			    tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
#endif
#if (defined(JEMALLOC_PROF) || defined(JEMALLOC_STATS))
		}
#endif
		deferred = NULL;
		ndeferred = 0;
		for (i = 0; i < nflush; i++) {
			ptr = flush;
			assert(ptr != NULL);
			flush = *(void **)ptr;
			chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
			if (chunk->arena == arena)
				arena_dalloc_large(arena, chunk, ptr);
			else {
				/*
				 * This object was allocated via a different
				 * arena than the one that is currently locked.
				 * Stash the object, so that it can be handled
				 * in a future pass.
				 */
				*(void **)ptr = deferred;
				deferred = ptr;
				ndeferred++;
			}
		}
		malloc_mutex_unlock(&arena->lock);

		if (flush != NULL) {
			/*
			 * This was the first pass, and rem cached objects
			 * remain.
			 */
			tbin->avail = flush;
		}
	}

	tbin->ncached = rem;
	if (tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
}

tcache_t *
tcache_create(arena_t *arena)
{
	tcache_t *tcache;
	size_t size;
	unsigned i;

	size = sizeof(tcache_t) + (sizeof(tcache_bin_t) * (nhbins - 1));
	/*
	 * Round up to the nearest multiple of the cacheline size, in order to
	 * avoid the possibility of false cacheline sharing.
	 *
	 * That this works relies on the same logic as in ipalloc().
	 */
	size = (size + CACHELINE_MASK) & (-CACHELINE);

	if (size <= small_maxclass)
		tcache = (tcache_t *)arena_malloc_small(arena, size, true);
	else
		tcache = (tcache_t *)icalloc(size);

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
	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	for (i = 0; i < nbins; i++) {
		if ((arena->bins[i].nregs << 1) <= TCACHE_NSLOTS_SMALL_MAX) {
			tcache->tbins[i].ncached_max = (arena->bins[i].nregs <<
			    1);
		} else
			tcache->tbins[i].ncached_max = TCACHE_NSLOTS_SMALL_MAX;
	}
	for (; i < nhbins; i++)
		tcache->tbins[i].ncached_max = TCACHE_NSLOTS_LARGE;

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
	malloc_mutex_unlock(&tcache->arena->lock);
	tcache_stats_merge(tcache, tcache->arena);
#endif

	for (i = 0; i < nbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_small(tbin, i, 0
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );

#ifdef JEMALLOC_STATS
		if (tbin->tstats.nrequests != 0) {
			arena_t *arena = tcache->arena;
			arena_bin_t *bin = &arena->bins[i];
			malloc_mutex_lock(&bin->lock);
			bin->stats.nrequests += tbin->tstats.nrequests;
			malloc_mutex_unlock(&bin->lock);
		}
#endif
	}

	for (; i < nhbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_large(tbin, i, 0
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );

#ifdef JEMALLOC_STATS
		if (tbin->tstats.nrequests != 0) {
			arena_t *arena = tcache->arena;
			malloc_mutex_lock(&arena->lock);
			arena->stats.nrequests_large += tbin->tstats.nrequests;
			arena->stats.lstats[i - nbins].nrequests +=
			    tbin->tstats.nrequests;
			malloc_mutex_unlock(&arena->lock);
		}
#endif
	}

#ifdef JEMALLOC_PROF
	if (tcache->prof_accumbytes > 0) {
		malloc_mutex_lock(&tcache->arena->lock);
		arena_prof_accum(tcache->arena, tcache->prof_accumbytes);
		malloc_mutex_unlock(&tcache->arena->lock);
	}
#endif

	if (arena_salloc(tcache) <= small_maxclass) {
		arena_chunk_t *chunk = CHUNK_ADDR2BASE(tcache);
		arena_t *arena = chunk->arena;
		size_t pageind = (((uintptr_t)tcache - (uintptr_t)chunk) >>
		    PAGE_SHIFT);
		arena_chunk_map_t *mapelm = &chunk->map[pageind];
		arena_run_t *run = (arena_run_t *)((uintptr_t)chunk +
		    (uintptr_t)((pageind - (mapelm->bits >> PAGE_SHIFT)) <<
		    PAGE_SHIFT));
		arena_bin_t *bin = run->bin;

		malloc_mutex_lock(&bin->lock);
		arena_dalloc_bin(arena, chunk, tcache, mapelm);
		malloc_mutex_unlock(&bin->lock);
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
	for (i = 0; i < nbins; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = &tcache->tbins[i];
		malloc_mutex_lock(&bin->lock);
		bin->stats.nrequests += tbin->tstats.nrequests;
		malloc_mutex_unlock(&bin->lock);
		tbin->tstats.nrequests = 0;
	}

	for (; i < nhbins; i++) {
		malloc_large_stats_t *lstats = &arena->stats.lstats[i - nbins];
		tcache_bin_t *tbin = &tcache->tbins[i];
		arena->stats.nrequests_large += tbin->tstats.nrequests;
		lstats->nrequests += tbin->tstats.nrequests;
		tbin->tstats.nrequests = 0;
	}
}
#endif

void
tcache_boot(void)
{

	if (opt_tcache) {
		/*
		 * If necessary, clamp opt_lg_tcache_maxclass, now that
		 * small_maxclass and arena_maxclass are known.
		 */
		if (opt_lg_tcache_maxclass < 0 || (1U <<
		    opt_lg_tcache_maxclass) < small_maxclass)
			tcache_maxclass = small_maxclass;
		else if ((1U << opt_lg_tcache_maxclass) > arena_maxclass)
			tcache_maxclass = arena_maxclass;
		else
			tcache_maxclass = (1U << opt_lg_tcache_maxclass);

		nhbins = nbins + (tcache_maxclass >> PAGE_SHIFT);

		/* Compute incremental GC event threshold. */
		if (opt_lg_tcache_gc_sweep >= 0) {
			tcache_gc_incr = ((1U << opt_lg_tcache_gc_sweep) /
			    nbins) + (((1U << opt_lg_tcache_gc_sweep) % nbins ==
			    0) ? 0 : 1);
		} else
			tcache_gc_incr = 0;

		if (pthread_key_create(&tcache_tsd, tcache_thread_cleanup) !=
		    0) {
			malloc_write(
			    "<jemalloc>: Error in pthread_key_create()\n");
			abort();
		}
	}
}
/******************************************************************************/
#endif /* JEMALLOC_TCACHE */
