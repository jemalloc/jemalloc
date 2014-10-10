/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	*huge_malloc(tsd_t *tsd, arena_t *arena, size_t size, bool zero,
    bool try_tcache);
void	*huge_palloc(tsd_t *tsd, arena_t *arena, size_t usize, size_t alignment,
    bool zero, bool try_tcache);
bool	huge_ralloc_no_move(void *ptr, size_t oldsize, size_t size,
    size_t extra, bool zero);
void	*huge_ralloc(tsd_t *tsd, arena_t *arena, void *ptr, size_t oldsize,
    size_t size, size_t extra, size_t alignment, bool zero,
    bool try_tcache_alloc, bool try_tcache_dalloc);
#ifdef JEMALLOC_JET
typedef void (huge_dalloc_junk_t)(void *, size_t);
extern huge_dalloc_junk_t *huge_dalloc_junk;
#endif
void	huge_dalloc(tsd_t *tsd, void *ptr, bool try_tcache);
size_t	huge_salloc(const void *ptr);
prof_tctx_t	*huge_prof_tctx_get(const void *ptr);
void	huge_prof_tctx_set(const void *ptr, prof_tctx_t *tctx);
bool	huge_boot(void);
void	huge_prefork(void);
void	huge_postfork_parent(void);
void	huge_postfork_child(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
