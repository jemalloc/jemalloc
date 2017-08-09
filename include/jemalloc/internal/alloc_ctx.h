#ifndef JEMALLOC_INTERNAL_ALLOC_CTX_H
#define JEMALLOC_INTERNAL_ALLOC_CTX_H

/* Used to pass rtree lookup context down the path. */
typedef struct alloc_ctx_s alloc_ctx_t;
struct alloc_ctx_s {
	szind_t szind;
	bool slab;
};

#endif /* JEMALLOC_INTERNAL_ALLOC_CTX_H */
