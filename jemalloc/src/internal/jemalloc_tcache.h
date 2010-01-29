#ifdef JEMALLOC_TCACHE
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct tcache_bin_s tcache_bin_t;
typedef struct tcache_s tcache_t;

/*
 * Default number of cache slots for each bin in the thread cache (0:
 * disabled).
 */
#define LG_TCACHE_NSLOTS_DEFAULT	7
 /*
  * (1U << opt_lg_tcache_gc_sweep) is the approximate number of allocation
  * events between full GC sweeps (-1: disabled).  Integer rounding may cause
  * the actual number to be slightly higher, since GC is performed
  * incrementally.
  */
#define LG_TCACHE_GC_SWEEP_DEFAULT	13

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct tcache_bin_s {
#  ifdef JEMALLOC_STATS
	tcache_bin_stats_t tstats;
#  endif
	unsigned	low_water;	/* Min # cached since last GC. */
	unsigned	high_water;	/* Max # cached since last GC. */
	unsigned	ncached;	/* # of cached objects. */
	void		*slots[1];	/* Dynamically sized. */
};

struct tcache_s {
#  ifdef JEMALLOC_STATS
	ql_elm(tcache_t) link;		/* Used for aggregating stats. */
#  endif
	arena_t		*arena;		/* This thread's arena. */
	unsigned	ev_cnt;		/* Event count since incremental GC. */
	unsigned	next_gc_bin;	/* Next bin to GC. */
	tcache_bin_t	*tbins[1];	/* Dynamically sized. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern size_t	opt_lg_tcache_nslots;
extern ssize_t	opt_lg_tcache_gc_sweep;

/* Map of thread-specific caches. */
extern __thread tcache_t	*tcache_tls
    JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Number of cache slots for each bin in the thread cache, or 0 if tcache is
 * disabled.
 */
extern size_t			tcache_nslots;

/* Number of tcache allocation/deallocation events between incremental GCs. */
extern unsigned			tcache_gc_incr;

void	tcache_bin_flush(tcache_bin_t *tbin, size_t binind, unsigned rem);
tcache_t *tcache_create(arena_t *arena);
void	tcache_bin_destroy(tcache_t *tcache, tcache_bin_t *tbin,
    unsigned binind);
void	*tcache_alloc_hard(tcache_t *tcache, tcache_bin_t *tbin, size_t binind);
tcache_bin_t *tcache_bin_create(arena_t *arena);
void	tcache_destroy(tcache_t *tcache);
#ifdef JEMALLOC_STATS
void	tcache_stats_merge(tcache_t *tcache, arena_t *arena);
#endif
void	tcache_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	tcache_event(tcache_t *tcache);
tcache_t *tcache_get(void);
void	*tcache_bin_alloc(tcache_bin_t *tbin);
void	*tcache_alloc(tcache_t *tcache, size_t size, bool zero);
void	tcache_dalloc(tcache_t *tcache, void *ptr);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_TCACHE_C_))
JEMALLOC_INLINE tcache_t *
tcache_get(void)
{
	tcache_t *tcache;

	if (isthreaded == false || tcache_nslots == 0)
		return (NULL);

	tcache = tcache_tls;
	if ((uintptr_t)tcache <= (uintptr_t)1) {
		if (tcache == NULL) {
			tcache = tcache_create(choose_arena());
			if (tcache == NULL)
				return (NULL);
		} else
			return (NULL);
	}

	return (tcache);
}

JEMALLOC_INLINE void
tcache_event(tcache_t *tcache)
{

	if (tcache_gc_incr == 0)
		return;

	tcache->ev_cnt++;
	assert(tcache->ev_cnt <= tcache_gc_incr);
	if (tcache->ev_cnt >= tcache_gc_incr) {
		size_t binind = tcache->next_gc_bin;
		tcache_bin_t *tbin = tcache->tbins[binind];

		if (tbin != NULL) {
			if (tbin->high_water == 0) {
				/*
				 * This bin went completely unused for an
				 * entire GC cycle, so throw away the tbin.
				 */
				assert(tbin->ncached == 0);
				tcache_bin_destroy(tcache, tbin, binind);
				tcache->tbins[binind] = NULL;
			} else {
				if (tbin->low_water > 0) {
					/*
					 * Flush (ceiling) half of the objects
					 * below the low water mark.
					 */
					tcache_bin_flush(tbin, binind,
					    tbin->ncached - (tbin->low_water >>
					    1) - (tbin->low_water & 1));
				}
				tbin->low_water = tbin->ncached;
				tbin->high_water = tbin->ncached;
			}
		}

		tcache->next_gc_bin++;
		if (tcache->next_gc_bin == nbins)
			tcache->next_gc_bin = 0;
		tcache->ev_cnt = 0;
	}
}

JEMALLOC_INLINE void *
tcache_bin_alloc(tcache_bin_t *tbin)
{

	if (tbin->ncached == 0)
		return (NULL);
	tbin->ncached--;
	if (tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
	return (tbin->slots[tbin->ncached]);
}

JEMALLOC_INLINE void *
tcache_alloc(tcache_t *tcache, size_t size, bool zero)
{
	void *ret;
	tcache_bin_t *tbin;
	size_t binind;

	if (size <= small_maxclass)
		binind = small_size2bin[size];
	else {
		binind = mbin0 + ((MEDIUM_CEILING(size) - medium_min) >>
		    lg_mspace);
	}
	assert(binind < nbins);
	tbin = tcache->tbins[binind];
	if (tbin == NULL) {
		tbin = tcache_bin_create(tcache->arena);
		if (tbin == NULL)
			return (NULL);
		tcache->tbins[binind] = tbin;
	}

	ret = tcache_bin_alloc(tbin);
	if (ret == NULL) {
		ret = tcache_alloc_hard(tcache, tbin, binind);
		if (ret == NULL)
			return (NULL);
	}

	if (zero == false) {
#ifdef JEMALLOC_FILL
		if (opt_junk)
			memset(ret, 0xa5, size);
		else if (opt_zero)
			memset(ret, 0, size);
#endif
	} else
		memset(ret, 0, size);

#ifdef JEMALLOC_STATS
	tbin->tstats.nrequests++;
#endif
	tcache_event(tcache);
	return (ret);
}

JEMALLOC_INLINE void
tcache_dalloc(tcache_t *tcache, void *ptr)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	arena_run_t *run;
	arena_bin_t *bin;
	tcache_bin_t *tbin;
	size_t pageind, binind;
	arena_chunk_map_t *mapelm;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    ((mapelm->bits & CHUNK_MAP_PG_MASK) >> CHUNK_MAP_PG_SHIFT)) <<
	    PAGE_SHIFT));
	assert(run->magic == ARENA_RUN_MAGIC);
	bin = run->bin;
	binind = ((uintptr_t)bin - (uintptr_t)&arena->bins) /
	    sizeof(arena_bin_t);
	assert(binind < nbins);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, arena->bins[binind].reg_size);
#endif

	tbin = tcache->tbins[binind];
	if (tbin == NULL) {
		tbin = tcache_bin_create(choose_arena());
		if (tbin == NULL) {
			malloc_mutex_lock(&arena->lock);
			arena_dalloc_bin(arena, chunk, ptr, mapelm);
			malloc_mutex_unlock(&arena->lock);
			return;
		}
		tcache->tbins[binind] = tbin;
	}

	if (tbin->ncached == tcache_nslots)
		tcache_bin_flush(tbin, binind, (tcache_nslots >> 1));
	assert(tbin->ncached < tcache_nslots);
	tbin->slots[tbin->ncached] = ptr;
	tbin->ncached++;
	if (tbin->ncached > tbin->high_water)
		tbin->high_water = tbin->ncached;

	tcache_event(tcache);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_TCACHE */
