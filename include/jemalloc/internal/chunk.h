/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/*
 * Size and alignment of memory chunks that are allocated by the OS's virtual
 * memory system.
 */
#define	LG_CHUNK_DEFAULT	21

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
void	*chunk_alloc_cache(arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool dalloc_node);
void	*chunk_alloc_default(void *new_addr, size_t size, size_t alignment,
    bool *zero, unsigned arena_ind);
void	*chunk_alloc_wrapper(arena_t *arena, chunk_alloc_t *chunk_alloc,
    void *new_addr, size_t size, size_t alignment, bool *zero);
void	chunk_record(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, bool cache, void *chunk, size_t size,
    bool zeroed);
void	chunk_dalloc_cache(arena_t *arena, void *chunk, size_t size);
void	chunk_dalloc_arena(arena_t *arena, void *chunk, size_t size,
    bool zeroed);
bool	chunk_dalloc_default(void *chunk, size_t size, unsigned arena_ind);
void	chunk_dalloc_wrapper(arena_t *arena, chunk_dalloc_t *chunk_dalloc,
    void *chunk, size_t size);
bool	chunk_purge_arena(arena_t *arena, void *chunk, size_t offset,
    size_t length);
bool	chunk_purge_default(void *chunk, size_t offset, size_t length,
    unsigned arena_ind);
bool	chunk_purge_wrapper(arena_t *arena, chunk_purge_t *chunk_purge,
    void *chunk, size_t offset, size_t length);
bool	chunk_boot(void);
void	chunk_prefork(void);
void	chunk_postfork_parent(void);
void	chunk_postfork_child(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
extent_node_t	*chunk_lookup(const void *chunk, bool dependent);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_CHUNK_C_))
JEMALLOC_INLINE extent_node_t *
chunk_lookup(const void *ptr, bool dependent)
{

	return (rtree_get(&chunks_rtree, (uintptr_t)ptr, dependent));
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#include "jemalloc/internal/chunk_dss.h"
#include "jemalloc/internal/chunk_mmap.h"
