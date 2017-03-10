#ifndef JEMALLOC_INTERNAL_MUTEX_TYPES_H
#define JEMALLOC_INTERNAL_MUTEX_TYPES_H

typedef struct lock_prof_data_s lock_prof_data_t;
typedef struct malloc_mutex_s malloc_mutex_t;

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

#define LOCK_PROF_DATA_INITIALIZER {0, 0, 0, 0, 0, 0, 0, NULL, 0}

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
/* TODO: get rid of adaptive mutex once we do our own spin. */
#  if (defined(JEMALLOC_HAVE_PTHREAD_MUTEX_ADAPTIVE_NP) &&		\
       defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP))
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_ADAPTIVE_NP
#    define MALLOC_MUTEX_INITIALIZER					\
       {{{LOCK_PROF_DATA_INITIALIZER,					\
          PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP}},			\
        WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#  else
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define MALLOC_MUTEX_INITIALIZER					\
       {{{LOCK_PROF_DATA_INITIALIZER, PTHREAD_MUTEX_INITIALIZER}},	\
        WITNESS_INITIALIZER("mutex", WITNESS_RANK_OMIT)}
#  endif
#endif

#endif /* JEMALLOC_INTERNAL_MUTEX_TYPES_H */
