/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct base_block_s base_block_t;
typedef struct base_s base_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

/* Embedded at the beginning of every block of base-managed virtual memory. */
struct base_block_s {
	/* Total size of block's virtual memory mapping. */
	size_t		size;

	/* Next block in list of base's blocks. */
	base_block_t	*next;

	/* Tracks unused trailing space. */
	extent_t	extent;
};

struct base_s {
	/* Associated arena's index within the arenas array. */
	unsigned	ind;

	/* User-configurable extent hook functions. */
	union {
		extent_hooks_t	*extent_hooks;
		void		*extent_hooks_pun;
	};

	/* Protects base_alloc() and base_stats_get() operations. */
	malloc_mutex_t	mtx;

	/* Serial number generation state. */
	size_t		extent_sn_next;

	/* Chain of all blocks associated with base. */
	base_block_t	*blocks;

	/* Heap of extents that track unused trailing space within blocks. */
	extent_heap_t	avail[NSIZES];

	/* Stats, only maintained if config_stats. */
	size_t		allocated;
	size_t		resident;
	size_t		mapped;
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

base_t	*b0get(void);
base_t	*base_new(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks);
void	base_delete(base_t *base);
extent_hooks_t	*base_extent_hooks_get(base_t *base);
extent_hooks_t	*base_extent_hooks_set(base_t *base,
    extent_hooks_t *extent_hooks);
void	*base_alloc(tsdn_t *tsdn, base_t *base, size_t size, size_t alignment);
void	base_stats_get(tsdn_t *tsdn, base_t *base, size_t *allocated,
    size_t *resident, size_t *mapped);
void	base_prefork(tsdn_t *tsdn, base_t *base);
void	base_postfork_parent(tsdn_t *tsdn, base_t *base);
void	base_postfork_child(tsdn_t *tsdn, base_t *base);
bool	base_boot(tsdn_t *tsdn);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
unsigned	base_ind_get(const base_t *base);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_BASE_C_))
JEMALLOC_INLINE unsigned
base_ind_get(const base_t *base)
{

	return (base->ind);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
