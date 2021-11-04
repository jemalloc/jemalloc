#ifndef JEMALLOC_INTERNAL_SAN_BUMP_H
#define JEMALLOC_INTERNAL_SAN_BUMP_H

#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/exp_grow.h"
#include "jemalloc/internal/mutex.h"

extern const size_t SBA_RETAINED_ALLOC_SIZE;

typedef struct ehooks_s ehooks_t;
typedef struct pac_s pac_t;

typedef struct san_bump_alloc_s san_bump_alloc_t;
struct san_bump_alloc_s {
	malloc_mutex_t mtx;

	edata_t *curr_reg;
};

bool
san_bump_alloc_init(san_bump_alloc_t* sba);

edata_t *
san_bump_alloc(tsdn_t *tsdn, san_bump_alloc_t* sba, pac_t *pac, ehooks_t *ehooks,
    size_t size, bool zero);

#endif /* JEMALLOC_INTERNAL_SAN_BUMP_H */
