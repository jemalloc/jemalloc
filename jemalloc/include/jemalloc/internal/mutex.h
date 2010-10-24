/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef pthread_mutex_t malloc_mutex_t;

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#  define MALLOC_MUTEX_INITIALIZER PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#else
#  define MALLOC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_LAZY_LOCK
extern bool isthreaded;
#else
#  define isthreaded true
#endif

bool	malloc_mutex_init(malloc_mutex_t *mutex);
void	malloc_mutex_destroy(malloc_mutex_t *mutex);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(malloc_mutex_t *mutex);
bool	malloc_mutex_trylock(malloc_mutex_t *mutex);
void	malloc_mutex_unlock(malloc_mutex_t *mutex);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_MUTEX_C_))
JEMALLOC_INLINE void
malloc_mutex_lock(malloc_mutex_t *mutex)
{

	if (isthreaded)
		pthread_mutex_lock(mutex);
}

JEMALLOC_INLINE bool
malloc_mutex_trylock(malloc_mutex_t *mutex)
{

	if (isthreaded)
		return (pthread_mutex_trylock(mutex) != 0);
	else
		return (false);
}

JEMALLOC_INLINE void
malloc_mutex_unlock(malloc_mutex_t *mutex)
{

	if (isthreaded)
		pthread_mutex_unlock(mutex);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
