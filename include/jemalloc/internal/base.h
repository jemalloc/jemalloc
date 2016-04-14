/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	*base_alloc(tsd_t *tsd, size_t size);
void	base_stats_get(tsd_t *tsd, size_t *allocated, size_t *resident,
    size_t *mapped);
bool	base_boot(void);
void	base_prefork(tsd_t *tsd);
void	base_postfork_parent(tsd_t *tsd);
void	base_postfork_child(tsd_t *tsd);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
