/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/*
 * Size and alignment of memory chunks that are allocated by the OS's virtual
 * memory system.
 */
#define	LG_CHUNK_DEFAULT	22

/* Return the chunk address for allocation address a. */
#define	CHUNK_ADDR2BASE(a)						\
	((void *)((uintptr_t)(a) & ~chunksize_mask))

/* Return the chunk offset of address a. */
#define	CHUNK_ADDR2OFFSET(a)						\
	((size_t)((uintptr_t)(a) & chunksize_mask))

/* Return the smallest chunk multiple that is >= s. */
#define	CHUNK_CEILING(s)						\
	(((s) + chunksize_mask) & ~chunksize_mask)

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_STATS
/* Chunk statistics. */
extern chunk_stats_t	stats_chunks;
#endif

extern size_t		opt_lg_chunk;
extern size_t		chunksize;
extern size_t		chunksize_mask; /* (chunksize - 1). */
extern size_t		chunk_npages;
extern size_t		arena_chunk_header_npages;
extern size_t		arena_maxclass; /* Max size class for arenas. */

#ifdef JEMALLOC_DSS
/*
 * Protects sbrk() calls.  This avoids malloc races among threads, though it
 * does not protect against races with threads that call sbrk() directly.
 */
extern malloc_mutex_t	dss_mtx;
/* Base address of the DSS. */
extern void		*dss_base;
/* Current end of the DSS, or ((void *)-1) if the DSS is exhausted. */
extern void		*dss_prev;
/* Current upper limit on DSS addresses. */
extern void		*dss_max;
#endif

void	*pages_map(void *addr, size_t size);
void	*chunk_alloc(size_t size, bool zero);
void	chunk_dealloc(void *chunk, size_t size);
bool	chunk_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
