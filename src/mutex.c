#define JEMALLOC_MUTEX_C_
#include "jemalloc/internal/jemalloc_internal.h"

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
#include <dlfcn.h>
#endif

#ifndef _CRT_SPINCOUNT
#define _CRT_SPINCOUNT 4000
#endif

/******************************************************************************/
/* Data. */

#ifdef JEMALLOC_LAZY_LOCK
bool isthreaded = false;
#endif
#ifdef JEMALLOC_MUTEX_INIT_CB
static bool		postpone_init = true;
static malloc_mutex_t	*postponed_mutexes = NULL;
#endif

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
static void	pthread_create_once(void);
#endif

/******************************************************************************/
/*
 * We intercept pthread_create() calls in order to toggle isthreaded if the
 * process goes multi-threaded.
 */

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
static int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_once(void) {
	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
	if (pthread_create_fptr == NULL) {
		malloc_write("<jemalloc>: Error in dlsym(RTLD_NEXT, "
		    "\"pthread_create\")\n");
		abort();
	}

	isthreaded = true;
}

JEMALLOC_EXPORT int
pthread_create(pthread_t *__restrict thread,
    const pthread_attr_t *__restrict attr, void *(*start_routine)(void *),
    void *__restrict arg) {
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	pthread_once(&once_control, pthread_create_once);

	return pthread_create_fptr(thread, attr, start_routine, arg);
}
#endif

/******************************************************************************/

#ifdef JEMALLOC_MUTEX_INIT_CB
JEMALLOC_EXPORT int	_pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
    void *(calloc_cb)(size_t, size_t));
#endif

void
malloc_mutex_lock_slow(malloc_mutex_t *mutex) {
	lock_prof_data_t *data = &mutex->prof_data;

	{//TODO: a smart spin policy
		if (!malloc_mutex_trylock(mutex)) {
			data->n_spin_acquired++;
			return;
		}
	}

	nstime_t now, before;
	nstime_init(&now, 0);
	nstime_update(&now);
	nstime_copy(&before, &now);

	uint32_t n_thds = atomic_add_u32(&data->n_waiting_thds, 1);
	/* One last try as above two calls may take quite some cycles. */
	if (!malloc_mutex_trylock(mutex)) {
		atomic_sub_u32(&data->n_waiting_thds, 1);
		data->n_spin_acquired++;
		return;
	}

	/* True slow path. */
	malloc_mutex_lock_final(mutex);
	atomic_sub_u32(&data->n_waiting_thds, 1);
	nstime_update(&now);

	/* Update more slow-path only counters. */
	nstime_subtract(&now, &before);
	uint64_t wait_time = nstime_ns(&now);
	data->n_wait_times++;
	data->tot_wait_time += wait_time;
	if (wait_time > data->max_wait_time) {
		data->max_wait_time = wait_time;
	}
	if (n_thds > data->max_n_thds) {
		data->max_n_thds = n_thds;
	}
}

static void
lock_prof_data_init(lock_prof_data_t *data) {
	memset(data, 0, sizeof(lock_prof_data_t));
	data->prev_owner = NULL;
}

bool
malloc_mutex_init(malloc_mutex_t *mutex, const char *name,
    witness_rank_t rank) {
	lock_prof_data_init(&mutex->prof_data);
#ifdef _WIN32
#  if _WIN32_WINNT >= 0x0600
	InitializeSRWLock(&mutex->lock);
#  else
	if (!InitializeCriticalSectionAndSpinCount(&mutex->lock,
	    _CRT_SPINCOUNT)) {
		return true;
	}
#  endif
#elif (defined(JEMALLOC_OS_UNFAIR_LOCK))
	mutex->lock = OS_UNFAIR_LOCK_INIT;
#elif (defined(JEMALLOC_OSSPIN))
	mutex->lock = 0;
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
	if (postpone_init) {
		mutex->postponed_next = postponed_mutexes;
		postponed_mutexes = mutex;
	} else {
		if (_pthread_mutex_init_calloc_cb(&mutex->lock,
		    bootstrap_calloc) != 0) {
			return true;
		}
	}
#else
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0) {
		return true;
	}
	pthread_mutexattr_settype(&attr, MALLOC_MUTEX_TYPE);
	if (pthread_mutex_init(&mutex->lock, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return true;
	}
	pthread_mutexattr_destroy(&attr);
#endif
	if (config_debug) {
		witness_init(&mutex->witness, name, rank, NULL, NULL);
	}
	return false;
}

void
malloc_mutex_prefork(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	malloc_mutex_lock(tsdn, mutex);
}

void
malloc_mutex_postfork_parent(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	malloc_mutex_unlock(tsdn, mutex);
}

void
malloc_mutex_postfork_child(tsdn_t *tsdn, malloc_mutex_t *mutex) {
#ifdef JEMALLOC_MUTEX_INIT_CB
	malloc_mutex_unlock(tsdn, mutex);
#else
	if (malloc_mutex_init(mutex, mutex->witness.name,
	    mutex->witness.rank)) {
		malloc_printf("<jemalloc>: Error re-initializing mutex in "
		    "child\n");
		if (opt_abort) {
			abort();
		}
	}
#endif
}

bool
malloc_mutex_boot(void) {
#ifdef JEMALLOC_MUTEX_INIT_CB
	postpone_init = false;
	while (postponed_mutexes != NULL) {
		if (_pthread_mutex_init_calloc_cb(&postponed_mutexes->lock,
		    bootstrap_calloc) != 0) {
			return true;
		}
		postponed_mutexes = postponed_mutexes->postponed_next;
	}
#endif
	return false;
}
