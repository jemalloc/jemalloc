#ifndef JEMALLOC_INTERNAL_ARENAS_MANAGEMENT_H
#define JEMALLOC_INTERNAL_ARENAS_MANAGEMENT_H

#include "jemalloc/internal/arena_types.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/tsd_types.h"

/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 *
 * arenas[0..narenas_auto) are used for automatic multiplexing of threads and
 * arenas.  arenas[narenas_auto..narenas_total) are only used if the application
 * takes some action to create them and allocate from them.
 *
 * Points to an arena_t.
 */
extern atomic_p_t arenas[];

/* Number of arenas used for automatic multiplexing of threads and arenas. */
extern unsigned narenas_auto;

/* Base index for manual arenas. */
extern unsigned manual_arena_base;

void     narenas_total_set(unsigned narenas);
void     narenas_total_inc(void);
unsigned narenas_total_get(void);
void     narenas_auto_set(unsigned n);
void     manual_arena_base_set(unsigned base);

void    *a0malloc(size_t size);
void     a0dalloc(void *ptr);
void    *a0ialloc(size_t size, bool zero, bool is_internal);
void     a0idalloc(void *ptr, bool is_internal);
void     arena_set(unsigned ind, arena_t *arena);
arena_t *arena_init(tsdn_t *tsdn, unsigned ind, const arena_config_t *config);
arena_t *arena_choose_hard(tsd_t *tsd, bool internal);
void     arena_migrate(tsd_t *tsd, arena_t *oldarena, arena_t *newarena);
void     iarena_cleanup(tsd_t *tsd);
void     arena_cleanup(tsd_t *tsd);

bool     arenas_management_boot(void);
void     arenas_management_prefork(tsdn_t *tsdn);
void     arenas_management_postfork_parent(tsdn_t *tsdn);
void     arenas_management_postfork_child(tsdn_t *tsdn);

#endif /* JEMALLOC_INTERNAL_ARENAS_MANAGEMENT_H */
