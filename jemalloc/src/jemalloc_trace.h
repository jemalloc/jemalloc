#ifdef JEMALLOC_TRACE
/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern bool	opt_trace;

void	trace_malloc(const void *ptr, size_t size);
void	trace_calloc(const void *ptr, size_t number, size_t size);
void	trace_posix_memalign(const void *ptr, size_t alignment, size_t size);
void	trace_realloc(const void *ptr, const void *old_ptr, size_t size,
    size_t old_size);
void	trace_free(const void *ptr, size_t size);
void	trace_malloc_usable_size(size_t size, const void *ptr);
void	trace_thread_exit(void);

void	trace_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
#endif /* JEMALLOC_TRACE */
