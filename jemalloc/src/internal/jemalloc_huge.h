/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_STATS
/* Huge allocation statistics. */
extern uint64_t		huge_nmalloc;
extern uint64_t		huge_ndalloc;
extern size_t		huge_allocated;
#endif

/* Protects chunk-related data structures. */
extern malloc_mutex_t	huge_mtx;

void	*huge_malloc(size_t size, bool zero);
void	*huge_palloc(size_t alignment, size_t size);
void	*huge_ralloc(void *ptr, size_t size, size_t oldsize);
void	huge_dalloc(void *ptr);
size_t	huge_salloc(const void *ptr);
#ifdef JEMALLOC_PROF
prof_thr_cnt_t	*huge_prof_cnt_get(const void *ptr);
void	huge_prof_cnt_set(const void *ptr, prof_thr_cnt_t *cnt);
#endif
bool	huge_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
