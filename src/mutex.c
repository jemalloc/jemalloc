#define JEMALLOC_MUTEX_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/spin.h"

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

static malloc_mutex_tree_t rbtmutexes_top_half = RB_INITIALIZER;
static malloc_mutex_tree_t rbtmutexes_bottom_half = RB_INITIALIZER;
static malloc_mutex_t rbt_mtx = MALLOC_MUTEX_INITIALIZER;
/******************************************************************************/
/*
 * We intercept pthread_create() calls in order to toggle isthreaded if the
 * process goes multi-threaded.
 */

#if defined(JEMALLOC_LAZY_LOCK) && !defined(_WIN32)
JEMALLOC_EXPORT int
pthread_create(pthread_t *__restrict thread,
    const pthread_attr_t *__restrict attr, void *(*start_routine)(void *),
    void *__restrict arg) {
	return pthread_create_wrapper(thread, attr, start_routine, arg);
}
#endif

/******************************************************************************/

#ifdef JEMALLOC_MUTEX_INIT_CB
JEMALLOC_EXPORT int	_pthread_mutex_init_calloc_cb(pthread_mutex_t *mutex,
    void *(calloc_cb)(size_t, size_t));
#endif

void
malloc_mutex_lock_slow(malloc_mutex_t *mutex) {
	mutex_prof_data_t *data = &mutex->prof_data;
	UNUSED nstime_t before = NSTIME_ZERO_INITIALIZER;

	if (ncpus == 1) {
		goto label_spin_done;
	}

	int cnt = 0, max_cnt = MALLOC_MUTEX_MAX_SPIN;
	do {
		spin_cpu_spinwait();
		if (!malloc_mutex_trylock_final(mutex)) {
			data->n_spin_acquired++;
			return;
		}
	} while (cnt++ < max_cnt);

	if (!config_stats) {
		/* Only spin is useful when stats is off. */
		malloc_mutex_lock_final(mutex);
		return;
	}
label_spin_done:
	nstime_update(&before);
	/* Copy before to after to avoid clock skews. */
	nstime_t after;
	nstime_copy(&after, &before);
	uint32_t n_thds = atomic_fetch_add_u32(&data->n_waiting_thds, 1,
	    ATOMIC_RELAXED) + 1;
	/* One last try as above two calls may take quite some cycles. */
	if (!malloc_mutex_trylock_final(mutex)) {
		atomic_fetch_sub_u32(&data->n_waiting_thds, 1, ATOMIC_RELAXED);
		data->n_spin_acquired++;
		return;
	}

	/* True slow path. */
	malloc_mutex_lock_final(mutex);
	/* Update more slow-path only counters. */
	atomic_fetch_sub_u32(&data->n_waiting_thds, 1, ATOMIC_RELAXED);
	nstime_update(&after);

	nstime_t delta;
	nstime_copy(&delta, &after);
	nstime_subtract(&delta, &before);

	data->n_wait_times++;
	nstime_add(&data->tot_wait_time, &delta);
	if (nstime_compare(&data->max_wait_time, &delta) < 0) {
		nstime_copy(&data->max_wait_time, &delta);
	}
	if (n_thds > data->max_n_thds) {
		data->max_n_thds = n_thds;
	}
}

static void
mutex_prof_data_init(mutex_prof_data_t *data) {
	memset(data, 0, sizeof(mutex_prof_data_t));
	nstime_init(&data->max_wait_time, 0);
	nstime_init(&data->tot_wait_time, 0);
	data->prev_owner = NULL;
}

void
malloc_mutex_prof_data_reset(tsdn_t *tsdn, malloc_mutex_t *mutex) {
	malloc_mutex_assert_owner(tsdn, mutex);
	mutex_prof_data_init(&mutex->prof_data);
}

static int
mutex_addr_comp(const witness_t *witness1, void *mutex1,
    const witness_t *witness2, void *mutex2) {
	assert(mutex1 != NULL);
	assert(mutex2 != NULL);
	uintptr_t mu1int = (uintptr_t)mutex1;
	uintptr_t mu2int = (uintptr_t)mutex2;
	if (mu1int < mu2int) {
		return -1;
	} else if (mu1int == mu2int) {
		return 0;
	} else {
		return 1;
	}
}

static int
malloc_mutex_rank_comp(const malloc_mutex_t *a, const malloc_mutex_t *b) {
	int ret = (a->witness.rank > b->witness.rank) - (a->witness.rank < b->witness.rank);
	if(ret != 0) {
		return ret;
	}
	if(a->witness.comp != NULL && a->witness.comp == b->witness.comp) {
		ret = a->witness.comp(&a->witness, a->witness.opaque, &b->witness, b->witness.opaque);
	}
	if(ret != 0) {
		return ret;
	}
	return (a > b) - (a < b);
}

rb_gen(static UNUSED, malloc_mutex_tree_, malloc_mutex_tree_t, malloc_mutex_t,
    rb_link, malloc_mutex_rank_comp)

static bool
malloc_mutex_init_internal(malloc_mutex_t *mutex, const char *name,
    witness_rank_t rank, malloc_mutex_lock_order_t lock_order, bool child_fork) {

	mutex_prof_data_init(&mutex->prof_data);
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
		mutex->lock_order = lock_order;
		if (lock_order == malloc_mutex_address_ordered) {
			witness_init(&mutex->witness, name, rank,
					mutex_addr_comp, child_fork ? mutex->witness.opaque : &mutex);
		} else {
			witness_init(&mutex->witness, name, rank, NULL, NULL);
		}
	}
	if(!child_fork) {
		malloc_mutex_lock(TSDN_NULL, &rbt_mtx);
#define OP(cmp_less_op, tree) do { \
			bool cond = cmp_less_op ? (rank < WITNESS_RANK_ARENAS) : (rank > WITNESS_RANK_ARENAS); \
			if(cond) { 	\
				malloc_mutex_t * tmp = malloc_mutex_tree_search(&tree, mutex);	\
				if(mutex != tmp) {	\
					malloc_mutex_tree_insert(&tree, mutex);	\
				}	\
			}	\
		} while(0)
		OP(true, rbtmutexes_top_half);
		OP(false, rbtmutexes_bottom_half);
#undef OP
		malloc_mutex_unlock(TSDN_NULL, &rbt_mtx);
	}
	return false;
}

bool
malloc_mutex_init(malloc_mutex_t *mutex, const char *name,
    witness_rank_t rank, malloc_mutex_lock_order_t lock_order) {
	return malloc_mutex_init_internal(mutex, name, rank, lock_order, false);
}

static malloc_mutex_t *
malloc_mutex_prefork_iter(malloc_mutex_tree_t * rbt, malloc_mutex_t * mutex, void * tsdn) {
	malloc_mutex_lock((tsdn_t *)tsdn, mutex);
	return NULL;
}

void
malloc_mutex_prefork(tsdn_t *tsdn, malloc_mutex_t * arenas_lock) {
	malloc_mutex_tree_iter(&rbtmutexes_top_half, NULL, malloc_mutex_prefork_iter, tsdn);
	malloc_mutex_lock(tsdn, arenas_lock);
	malloc_mutex_tree_iter(&rbtmutexes_bottom_half, NULL, malloc_mutex_prefork_iter, tsdn);
}

static malloc_mutex_t *
malloc_mutex_postfork_unlock_iter(malloc_mutex_tree_t * rbt, malloc_mutex_t * mutex, void * tsdn) {
	malloc_mutex_unlock((tsdn_t *)tsdn, mutex);
	return NULL;
}

static malloc_mutex_t *
malloc_mutex_postfork_init_iter(malloc_mutex_tree_t * rbt, malloc_mutex_t * mutex, void * tsdn) {
	if (malloc_mutex_init_internal(mutex, mutex->witness.name,
	    mutex->witness.rank, mutex->lock_order, true)) {
		malloc_printf("<jemalloc>: Error re-initializing mutex in "
		    "child\n");
		if (opt_abort) {
			abort();
		}
	}
	return NULL;
}

void
malloc_mutex_postfork_parent(tsdn_t *tsdn, malloc_mutex_t * arenas_lock) {
	malloc_mutex_tree_reverse_iter(&rbtmutexes_top_half, NULL, malloc_mutex_postfork_unlock_iter, tsdn);

	malloc_mutex_lock(TSDN_NULL, &rbt_mtx);

	//arenas unlock allows new arenas to be added with new
	//mutexes, which modifies rbtmutexes_bottom_half tree, so we need to be
	//inside lock
	malloc_mutex_unlock(tsdn, arenas_lock);
	malloc_mutex_tree_reverse_iter(&rbtmutexes_bottom_half, NULL, malloc_mutex_postfork_unlock_iter, tsdn);

	malloc_mutex_unlock(TSDN_NULL, &rbt_mtx);
}

void
malloc_mutex_postfork_child(tsdn_t *tsdn, malloc_mutex_t * arenas_lock) {
	malloc_mutex_init_internal(&rbt_mtx, rbt_mtx.witness.name,
			rbt_mtx.witness.rank, rbt_mtx.lock_order, true);
#ifdef JEMALLOC_MUTEX_INIT_CB
	malloc_mutex_tree_iter(&rbtmutexes_top_half, NULL, malloc_mutex_postfork_unlock_iter, tsdn);

	malloc_mutex_lock(TSDN_NULL, &rbt_mtx);
	malloc_mutex_unlock(tsdn, arenas_lock);
	malloc_mutex_tree_iter(&rbtmutexes_bottom_half, NULL, malloc_mutex_postfork_unlock_iter, tsdn);
	malloc_mutex_unlock(TSDN_NULL, &rbt_mtx);
#else
	malloc_mutex_tree_iter(&rbtmutexes_top_half, NULL, malloc_mutex_postfork_init_iter, tsdn);

	malloc_mutex_lock(TSDN_NULL, &rbt_mtx);
	malloc_mutex_init_internal(arenas_lock, arenas_lock->witness.name,
			arenas_lock->witness.rank, arenas_lock->lock_order, true);
	malloc_mutex_tree_iter(&rbtmutexes_bottom_half, NULL, malloc_mutex_postfork_init_iter, tsdn);
	malloc_mutex_unlock(TSDN_NULL, &rbt_mtx);
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
	malloc_mutex_init_internal(&rbt_mtx, "mutex_rbtree_mtx", WITNESS_RANK_OMIT,
			malloc_mutex_rank_exclusive, true);
	return false;
}

void malloc_mutex_rbtree_remove(tsdn_t *tsdn, malloc_mutex_t * mutex) {
	malloc_mutex_lock(TSDN_NULL, &rbt_mtx);
#define OP(tree, mutex) do {	\
		malloc_mutex_t * tmp = malloc_mutex_tree_search(&tree, mutex);	\
		if(tmp == mutex) {	\
			malloc_mutex_tree_remove(&tree, mutex);	\
		}	\
	} while(0)
	OP(rbtmutexes_top_half, mutex);
	OP(rbtmutexes_bottom_half, mutex);
#undef OP
	malloc_mutex_unlock(TSDN_NULL, &rbt_mtx);	\
}
