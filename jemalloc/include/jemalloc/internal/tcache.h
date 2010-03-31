#ifdef JEMALLOC_TCACHE
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct tcache_bin_s tcache_bin_t;
typedef struct tcache_s tcache_t;

/*
 * Absolute maximum number of cache slots for each small bin in the thread
 * cache.  This is an additional constraint beyond that imposed as: twice the
 * number of regions per run for this size class.
 *
 * This constant must be an even number.
 */
#define	TCACHE_NSLOTS_SMALL_MAX		200

/* Number of cache slots for large size classes. */
#define	TCACHE_NSLOTS_LARGE		20

/* (1U << opt_lg_tcache_maxclass) is used to compute tcache_maxclass. */
#define	LG_TCACHE_MAXCLASS_DEFAULT	15

/*
 * (1U << opt_lg_tcache_gc_sweep) is the approximate number of allocation
 * events between full GC sweeps (-1: disabled).  Integer rounding may cause
 * the actual number to be slightly higher, since GC is performed
 * incrementally.
 */
#define	LG_TCACHE_GC_SWEEP_DEFAULT	13

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
	unsigned	ncached_max;	/* Upper limit on ncached. */
	void		*avail;		/* Chain of available objects. */
};

struct tcache_s {
#  ifdef JEMALLOC_STATS
	ql_elm(tcache_t) link;		/* Used for aggregating stats. */
#  endif
#  ifdef JEMALLOC_PROF
	uint64_t	prof_accumbytes;/* Cleared after arena_prof_accum() */
#  endif
	arena_t		*arena;		/* This thread's arena. */
	unsigned	ev_cnt;		/* Event count since incremental GC. */
	unsigned	next_gc_bin;	/* Next bin to GC. */
	tcache_bin_t	tbins[1];	/* Dynamically sized. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern bool	opt_tcache;
extern ssize_t	opt_lg_tcache_maxclass;
extern ssize_t	opt_lg_tcache_gc_sweep;

/* Map of thread-specific caches. */
extern __thread tcache_t	*tcache_tls
    JEMALLOC_ATTR(tls_model("initial-exec"));

/*
 * Number of tcache bins.  There are nbins small-object bins, plus 0 or more
 * large-object bins.
 */
extern size_t			nhbins;

/* Maximum cached size class. */
extern size_t			tcache_maxclass;

/* Number of tcache allocation/deallocation events between incremental GCs. */
extern unsigned			tcache_gc_incr;

void	tcache_bin_flush_small(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    );
void	tcache_bin_flush_large(tcache_bin_t *tbin, size_t binind, unsigned rem
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
    , tcache_t *tcache
#endif
    );
tcache_t *tcache_create(arena_t *arena);
void	*tcache_alloc_small_hard(tcache_t *tcache, tcache_bin_t *tbin,
    size_t binind);
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
void	*tcache_alloc_easy(tcache_bin_t *tbin);
void	*tcache_alloc_small(tcache_t *tcache, size_t size, bool zero);
void	*tcache_alloc_large(tcache_t *tcache, size_t size, bool zero);
void	tcache_dalloc_small(tcache_t *tcache, void *ptr);
void	tcache_dalloc_large(tcache_t *tcache, void *ptr, size_t size);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_TCACHE_C_))
JEMALLOC_INLINE tcache_t *
tcache_get(void)
{
	tcache_t *tcache;

	if ((isthreaded & opt_tcache) == false)
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
	if (tcache->ev_cnt == tcache_gc_incr) {
		size_t binind = tcache->next_gc_bin;
		tcache_bin_t *tbin = &tcache->tbins[binind];

		if (tbin->low_water > 0) {
			/*
			 * Flush (ceiling) 3/4 of the objects below the low
			 * water mark.
			 */
			if (binind < nbins) {
				tcache_bin_flush_small(tbin, binind,
				    tbin->ncached - tbin->low_water +
				    (tbin->low_water >> 2)
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
				    , tcache
#endif
				    );
			} else {
				tcache_bin_flush_large(tbin, binind,
				    tbin->ncached - tbin->low_water +
				    (tbin->low_water >> 2)
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
				    , tcache
#endif
				    );
			}
		}
		tbin->low_water = tbin->ncached;
		tbin->high_water = tbin->ncached;

		tcache->next_gc_bin++;
		if (tcache->next_gc_bin == nhbins)
			tcache->next_gc_bin = 0;
		tcache->ev_cnt = 0;
	}
}

JEMALLOC_INLINE void *
tcache_alloc_easy(tcache_bin_t *tbin)
{
	void *ret;

	if (tbin->ncached == 0)
		return (NULL);
	tbin->ncached--;
	if (tbin->ncached < tbin->low_water)
		tbin->low_water = tbin->ncached;
	ret = tbin->avail;
	tbin->avail = *(void **)ret;
	return (ret);
}

JEMALLOC_INLINE void *
tcache_alloc_small(tcache_t *tcache, size_t size, bool zero)
{
	void *ret;
	size_t binind;
	tcache_bin_t *tbin;

	binind = small_size2bin[size];
	assert(binind < nbins);
	tbin = &tcache->tbins[binind];
	ret = tcache_alloc_easy(tbin);
	if (ret == NULL) {
		ret = tcache_alloc_small_hard(tcache, tbin, binind);
		if (ret == NULL)
			return (NULL);
	}
	assert(arena_salloc(ret) == tcache->arena->bins[binind].reg_size);

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
#ifdef JEMALLOC_PROF
	tcache->prof_accumbytes += tcache->arena->bins[binind].reg_size;
#endif
	tcache_event(tcache);
	return (ret);
}

JEMALLOC_INLINE void *
tcache_alloc_large(tcache_t *tcache, size_t size, bool zero)
{
	void *ret;
	size_t binind;
	tcache_bin_t *tbin;

	size = PAGE_CEILING(size);
	assert(size <= tcache_maxclass);
	binind = nbins + (size >> PAGE_SHIFT) - 1;
	assert(binind < nhbins);
	tbin = &tcache->tbins[binind];
	ret = tcache_alloc_easy(tbin);
	if (ret == NULL) {
		/*
		 * Only allocate one large object at a time, because it's quite
		 * expensive to create one and not use it.
		 */
		ret = arena_malloc_large(tcache->arena, size, zero);
		if (ret == NULL)
			return (NULL);
	} else {
#ifdef JEMALLOC_PROF
		arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ret);
		size_t pageind = (unsigned)(((uintptr_t)ret - (uintptr_t)chunk)
		    >> PAGE_SHIFT);
		chunk->map[pageind].bits |= CHUNK_MAP_CLASS_MASK;
#endif
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
#ifdef JEMALLOC_PROF
		tcache->prof_accumbytes += size;
#endif
	}

	tcache_event(tcache);
	return (ret);
}

JEMALLOC_INLINE void
tcache_dalloc_small(tcache_t *tcache, void *ptr)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	arena_run_t *run;
	arena_bin_t *bin;
	tcache_bin_t *tbin;
	size_t pageind, binind;
	arena_chunk_map_t *mapelm;

	assert(arena_salloc(ptr) <= small_maxclass);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];
	run = (arena_run_t *)((uintptr_t)chunk + (uintptr_t)((pageind -
	    (mapelm->bits >> PAGE_SHIFT)) << PAGE_SHIFT));
	assert(run->magic == ARENA_RUN_MAGIC);
	bin = run->bin;
	binind = ((uintptr_t)bin - (uintptr_t)&arena->bins) /
	    sizeof(arena_bin_t);
	assert(binind < nbins);

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, bin->reg_size);
#endif

	tbin = &tcache->tbins[binind];
	if (tbin->ncached == tbin->ncached_max) {
		tcache_bin_flush_small(tbin, binind, (tbin->ncached_max >> 1)
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );
	}
	assert(tbin->ncached < tbin->ncached_max);
	*(void **)ptr = tbin->avail;
	tbin->avail = ptr;
	tbin->ncached++;
	if (tbin->ncached > tbin->high_water)
		tbin->high_water = tbin->ncached;

	tcache_event(tcache);
}

JEMALLOC_INLINE void
tcache_dalloc_large(tcache_t *tcache, void *ptr, size_t size)
{
	arena_t *arena;
	arena_chunk_t *chunk;
	size_t pageind, binind;
	tcache_bin_t *tbin;
	arena_chunk_map_t *mapelm;

	assert((size & PAGE_MASK) == 0);
	assert(arena_salloc(ptr) > small_maxclass);
	assert(arena_salloc(ptr) <= tcache_maxclass);

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	arena = chunk->arena;
	pageind = (((uintptr_t)ptr - (uintptr_t)chunk) >> PAGE_SHIFT);
	mapelm = &chunk->map[pageind];
	binind = nbins + (size >> PAGE_SHIFT) - 1;

#ifdef JEMALLOC_FILL
	if (opt_junk)
		memset(ptr, 0x5a, bin->reg_size);
#endif

	tbin = &tcache->tbins[binind];
	if (tbin->ncached == tbin->ncached_max) {
		tcache_bin_flush_large(tbin, binind, (tbin->ncached_max >> 1)
#if (defined(JEMALLOC_STATS) || defined(JEMALLOC_PROF))
		    , tcache
#endif
		    );
	}
	assert(tbin->ncached < tbin->ncached_max);
	*(void **)ptr = tbin->avail;
	tbin->avail = ptr;
	tbin->ncached++;
	if (tbin->ncached > tbin->high_water)
		tbin->high_water = tbin->ncached;

	tcache_event(tcache);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_TCACHE */
