#ifndef JEMALLOC_INTERNAL_MUTEX_TYPES_H
#define JEMALLOC_INTERNAL_MUTEX_TYPES_H

typedef struct malloc_mutex_s malloc_mutex_t;

/*
 * Based on benchmark results, a fixed spin with this amount of retries works
 * well for our critical sections.
 */
#define MALLOC_MUTEX_MAX_SPIN 250

#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
#    define MALLOC_MUTEX_LOCK(m)    AcquireSRWLockExclusive(&(m)->lock)
#    define MALLOC_MUTEX_UNLOCK(m)  ReleaseSRWLockExclusive(&(m)->lock)
#    define MALLOC_MUTEX_TRYLOCK(m) (!TryAcquireSRWLockExclusive(&(m)->lock))
#  else
#    define MALLOC_MUTEX_LOCK(m)    EnterCriticalSection(&(m)->lock)
#    define MALLOC_MUTEX_UNLOCK(m)  LeaveCriticalSection(&(m)->lock)
#    define MALLOC_MUTEX_TRYLOCK(m) (!TryEnterCriticalSection(&(m)->lock))
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
#    define MALLOC_MUTEX_LOCK(m)    os_unfair_lock_lock(&(m)->lock)
#    define MALLOC_MUTEX_UNLOCK(m)  os_unfair_lock_unlock(&(m)->lock)
#    define MALLOC_MUTEX_TRYLOCK(m) (!os_unfair_lock_trylock(&(m)->lock))
#elif (defined(JEMALLOC_OSSPIN))
#    define MALLOC_MUTEX_LOCK(m)    OSSpinLockLock(&(m)->lock)
#    define MALLOC_MUTEX_UNLOCK(m)  OSSpinLockUnlock(&(m)->lock)
#    define MALLOC_MUTEX_TRYLOCK(m) (!OSSpinLockTry(&(m)->lock))
#else
#    define MALLOC_MUTEX_LOCK(m)    pthread_mutex_lock(&(m)->lock)
#    define MALLOC_MUTEX_UNLOCK(m)  pthread_mutex_unlock(&(m)->lock)
#    define MALLOC_MUTEX_TRYLOCK(m) (pthread_mutex_trylock(&(m)->lock) != 0)
#endif

#define LOCK_PROF_DATA_INITIALIZER					\
    {NSTIME_ZERO_INITIALIZER, NSTIME_ZERO_INITIALIZER, 0, 0, 0,		\
	    ATOMIC_INIT(0), 0, NULL, 0}

#ifdef _WIN32
#  define MALLOC_MUTEX_INITIALIZER
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
#  define MALLOC_MUTEX_INITIALIZER					\
     {{{LOCK_PROF_DATA_INITIALIZER, OS_UNFAIR_LOCK_INIT}},		\
      WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#elif (defined(JEMALLOC_OSSPIN))
#  define MALLOC_MUTEX_INITIALIZER					\
     {{{LOCK_PROF_DATA_INITIALIZER, 0}},				\
      WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
#  define MALLOC_MUTEX_INITIALIZER					\
     {{{LOCK_PROF_DATA_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, NULL}},	\
      WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#else
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define MALLOC_MUTEX_INITIALIZER					\
       {{{LOCK_PROF_DATA_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}},	\
        WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#endif

#endif /* JEMALLOC_INTERNAL_MUTEX_TYPES_H */
