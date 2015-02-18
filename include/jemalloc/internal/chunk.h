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

extern size_t		opt_lg_chunk;
extern const char	*opt_dss;

extern rtree_t		chunks_rtree;

extern size_t		chunksize;
extern size_t		chunksize_mask; /* (chunksize - 1). */
extern size_t		chunk_npages;

bool	chunk_register(const void *chunk, const extent_node_t *node);
void	chunk_deregister(const void *chunk, const extent_node_t *node);
void	*chunk_alloc_base(size_t size);
void	*chunk_alloc_arena(chunk_alloc_t *chunk_alloc,
    chunk_dalloc_t *chunk_dalloc, unsigned arena_ind, void *new_addr,
    size_t size, size_t alignment, bool *zero);
void	*chunk_alloc_default(void *new_addr, size_t size, size_t alignment,
    bool *zero, unsigned arena_ind);
void	chunk_record(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, bool dirty, void *chunk, size_t size);
bool	chunk_dalloc_default(void *chunk, size_t size, unsigned arena_ind);
void	chunk_unmap(arena_t *arena, bool dirty, void *chunk, size_t size);
bool	chunk_boot(void);
void	chunk_prefork(void);
void	chunk_postfork_parent(void);
void	chunk_postfork_child(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
extent_node_t	*chunk_lookup(const void *chunk);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_CHUNK_C_))
JEMALLOC_INLINE extent_node_t *
chunk_lookup(const void *chunk)
{

	return (rtree_get(&chunks_rtree, (uintptr_t)chunk));
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#include "jemalloc/internal/chunk_dss.h"
#include "jemalloc/internal/chunk_mmap.h"
