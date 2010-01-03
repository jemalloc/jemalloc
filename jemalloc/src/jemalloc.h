#ifndef JEMALLOC_H_
#define	JEMALLOC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "jemalloc_defs.h"

size_t	malloc_usable_size(const void *ptr);

#ifdef JEMALLOC_TCACHE
void	malloc_tcache_flush(void);
#endif

extern const char	*malloc_options;
extern void		(*malloc_message)(const char *p1,
    const char *p2, const char *p3, const char *p4);

#ifdef __cplusplus
};
#endif
#endif /* JEMALLOC_H_ */
