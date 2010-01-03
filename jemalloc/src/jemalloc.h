#ifndef JEMALLOC_H_
#define	JEMALLOC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "jemalloc_defs.h"

extern const char	*malloc_options;
extern void		(*malloc_message)(const char *p1,
    const char *p2, const char *p3, const char *p4);

void	*malloc(size_t size) JEMALLOC_ATTR(malloc);
void	*calloc(size_t num, size_t size) JEMALLOC_ATTR(malloc);
int	posix_memalign(void **memptr, size_t alignment, size_t size)
    JEMALLOC_ATTR(nonnull(1));
void	*realloc(void *ptr, size_t size);
void	free(void *ptr);

size_t	malloc_usable_size(const void *ptr);
#ifdef JEMALLOC_TCACHE
void	malloc_tcache_flush(void);
#endif
void	malloc_stats_print(const char *opts);

#ifdef __cplusplus
};
#endif
#endif /* JEMALLOC_H_ */
