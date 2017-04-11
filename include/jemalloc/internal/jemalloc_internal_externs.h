#ifndef JEMALLOC_INTERNAL_EXTERNS_H
#define JEMALLOC_INTERNAL_EXTERNS_H

#include "jemalloc/internal/atomic.h"

extern bool	opt_abort;
extern const char	*opt_junk;
extern bool	opt_junk_alloc;
extern bool	opt_junk_free;
extern bool	opt_utrace;
extern bool	opt_xmalloc;
extern bool	opt_zero;
extern unsigned	opt_narenas;

/* Number of CPUs. */
extern unsigned	ncpus;

/* Number of arenas used for automatic multiplexing of threads and arenas. */
extern unsigned	narenas_auto;

/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 */
extern atomic_p_t arenas[];

/*
 * pind2sz_tab encodes the same information as could be computed by
 * pind2sz_compute().
 */
extern size_t const	pind2sz_tab[NPSIZES+1];
/*
 * index2size_tab encodes the same information as could be computed (at
 * unacceptable cost in some code paths) by index2size_compute().
 */
extern size_t const	index2size_tab[NSIZES];
/*
 * size2index_tab is a compact lookup table that rounds request sizes up to
 * size classes.  In order to reduce cache footprint, the table is compressed,
 * and all accesses are via size2index().
 */
extern uint8_t const	size2index_tab[];

void *a0malloc(size_t size);
void a0dalloc(void *ptr);
void *bootstrap_malloc(size_t size);
void *bootstrap_calloc(size_t num, size_t size);
void bootstrap_free(void *ptr);
void arena_set(unsigned ind, arena_t *arena);
unsigned narenas_total_get(void);
arena_t *arena_init(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks);
arena_tdata_t *arena_tdata_get_hard(tsd_t *tsd, unsigned ind);
arena_t *arena_choose_hard(tsd_t *tsd, bool internal);
void arena_migrate(tsd_t *tsd, unsigned oldind, unsigned newind);
void iarena_cleanup(tsd_t *tsd);
void arena_cleanup(tsd_t *tsd);
void arenas_tdata_cleanup(tsd_t *tsd);
void jemalloc_prefork(void);
void jemalloc_postfork_parent(void);
void jemalloc_postfork_child(void);
bool malloc_initialized(void);

#endif /* JEMALLOC_INTERNAL_EXTERNS_H */
