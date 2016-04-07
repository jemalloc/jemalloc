/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/* Page size.  LG_PAGE is determined by the configure script. */
#ifdef PAGE_MASK
#  undef PAGE_MASK
#endif
#define	PAGE		((size_t)(1U << LG_PAGE))
#define	PAGE_MASK	((size_t)(PAGE - 1))

/* Return the page base address for the page containing address a. */
#define	PAGE_ADDR2BASE(a)						\
	((void *)((uintptr_t)(a) & ~PAGE_MASK))

/* Return the smallest pagesize multiple that is >= s. */
#define	PAGE_CEILING(s)							\
	(((s) + PAGE_MASK) & ~PAGE_MASK)

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

void	*pages_map(void *addr, size_t size, bool *commit);
void	pages_unmap(void *addr, size_t size);
void	*pages_trim(void *addr, size_t alloc_size, size_t leadsize,
    size_t size, bool *commit);
bool	pages_commit(void *addr, size_t size);
bool	pages_decommit(void *addr, size_t size);
bool	pages_purge(void *addr, size_t size);
void	pages_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

