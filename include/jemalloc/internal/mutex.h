/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

#ifdef JEMALLOC_OSSPIN
typedef OSSpinLock malloc_mutex_t;
#define	MALLOC_MUTEX_INITIALIZER 0
#else
typedef pthread_mutex_t malloc_mutex_t;
#  if (defined(PTHREAD_MUTEX_ADAPTIVE_NP) &&				\
       defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP))
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_ADAPTIVE_NP
#    define MALLOC_MUTEX_INITIALIZER PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#  else
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define MALLOC_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#  endif
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
void	malloc_mutex_prefork(malloc_mutex_t *mutex);
void	malloc_mutex_postfork_parent(malloc_mutex_t *mutex);
void	malloc_mutex_postfork_child(malloc_mutex_t *mutex);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(malloc_mutex_t *mutex);
void	malloc_mutex_unlock(malloc_mutex_t *mutex);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_MUTEX_C_))
JEMALLOC_INLINE void
malloc_mutex_lock(malloc_mutex_t *mutex)
{

	if (isthreaded) {
#ifdef JEMALLOC_OSSPIN
		OSSpinLockLock(mutex);
#else
		pthread_mutex_lock(mutex);
#endif
	}
}

JEMALLOC_INLINE void
malloc_mutex_unlock(malloc_mutex_t *mutex)
{

	if (isthreaded) {
#ifdef JEMALLOC_OSSPIN
		OSSpinLockUnlock(mutex);
#else
		pthread_mutex_unlock(mutex);
#endif
	}
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
