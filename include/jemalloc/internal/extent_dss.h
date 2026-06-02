#ifndef JEMALLOC_INTERNAL_EXTENT_DSS_H
#define JEMALLOC_INTERNAL_EXTENT_DSS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/tsd_types.h"

/* Forward decl; arena.h includes us, so we can't include arena.h back. */
typedef struct arena_s arena_t;

typedef enum {
	dss_prec_disabled = 0,
	dss_prec_primary = 1,
	dss_prec_secondary = 2,

	dss_prec_limit = 3
} dss_prec_t;
#define DSS_PREC_DEFAULT dss_prec_secondary
#define DSS_DEFAULT "secondary"

extern const char *const dss_prec_names[];

extern const char *opt_dss;

dss_prec_t extent_dss_prec_get(void);
bool       extent_dss_prec_set(dss_prec_t dss_prec);
void      *extent_alloc_dss(tsdn_t *tsdn, arena_t *arena, void *new_addr,
         size_t size, size_t alignment, bool *zero, bool *commit);
bool       extent_in_dss(void *addr);
bool       extent_dss_mergeable(void *addr_a, void *addr_b);
void       extent_dss_boot(void);

#ifdef JEMALLOC_JET
typedef void *(*extent_dss_sbrk_hook_t)(intptr_t);
extern extent_dss_sbrk_hook_t extent_dss_sbrk_hook;
#endif

#endif /* JEMALLOC_INTERNAL_EXTENT_DSS_H */
