#ifndef JEMALLOC_H_
#define	JEMALLOC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "jemalloc_defs.h"
#ifndef JEMALLOC_P
#  define JEMALLOC_P(s) s
#endif

extern const char	*JEMALLOC_P(malloc_options);
extern void		(*JEMALLOC_P(malloc_message))(const char *p1,
    const char *p2, const char *p3, const char *p4);

void	*JEMALLOC_P(malloc)(size_t size) JEMALLOC_ATTR(malloc);
void	*JEMALLOC_P(calloc)(size_t num, size_t size) JEMALLOC_ATTR(malloc);
int	JEMALLOC_P(posix_memalign)(void **memptr, size_t alignment, size_t size)
    JEMALLOC_ATTR(nonnull(1));
void	*JEMALLOC_P(realloc)(void *ptr, size_t size);
void	JEMALLOC_P(free)(void *ptr);

size_t	JEMALLOC_P(malloc_usable_size)(const void *ptr);
#ifdef JEMALLOC_TCACHE
void	JEMALLOC_P(malloc_tcache_flush)(void);
#endif
void	JEMALLOC_P(malloc_stats_print)(const char *opts);

#ifdef __cplusplus
};
#endif
#endif /* JEMALLOC_H_ */
