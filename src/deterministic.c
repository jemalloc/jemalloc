#define JEMALLOC_DETERMINISTIC_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#if defined(JEMALLOC_DETERMINISTIC_SCHED)

#include <semaphore.h>
#include <pthread.h>

/*
 * This file implements a deterministic scheduler used for testing.
 * It instruments atomics and mutexes in jemalloc, and tests using it
 * attempt to fully explore the state-space of various atomic access
 * and mutex acquisitions.  Race conditions that might otherwise
 * only show up in hours show up within minutes of testing.
 *
 * Usage: (must be built with --enable-debug)
 * 	det_start(atoi(getenv("SEED")), numthreads, &testthread, NULL);
 *
 * Where testthread implements any current testing necessary.  No
 * det_* specific calls are necessary in testthread (only usage of
 * jemalloc's mutex and atomics classes).  If you do have any
 * additional synchronized state, wrap it in det_before_shared() and
 * det_after_shared(), like a lock.
 */

#define MAX_THREADS 100

struct waitlist_s {
	unsigned id;
	struct waitlist_s* next;
	sem_t sem;
};

/* Our sem and id. Must be TLS, because it indexes
 * in to the other arrays.
 */
static __thread sem_t* tls_sem = NULL;
static __thread unsigned id;

/* Number of assigned threads. */
static unsigned nextid = 0;
/* Sems.  Used before pthread_create for new threads, so can't be
 * TLS.
 */
static sem_t sems[MAX_THREADS];
/* State used for selecting next thread to wake.  Seed is set at init
 * time.  The state is deterministic based on seed.
 */
static uint64_t rand_state;

/* Threads actually waiting for scheduling. Threads are removed from
 * here if they are waiting on a mutex, or they are waiting to
 * pthread_join.
 *
 * Threads are *not* removed if they are already running - it just
 * means the same thread is activated twice in a row.
 */
static sem_t *sems_active[MAX_THREADS];
static unsigned active_count;

/* Assertions that no extraneous calls to det_* functions happen after
 * we have started the schedule
 */
static bool started = false;

struct barrier_s {
	pthread_mutex_t m;
	pthread_cond_t c;
	unsigned num;
	unsigned gen;
	unsigned count;
};

typedef struct barrier_s barrier_t;

/* Used for starting joining threads */
static barrier_t thread_barrier;

static void barrier_init(barrier_t* barrier, unsigned count) {
	pthread_mutex_init(&barrier->m, NULL);
	pthread_cond_init(&barrier->c, NULL);
	barrier->count = count;
	barrier->gen = 0;
	barrier->num = 0;
}

static void barrier_wait(barrier_t* barrier) {
	pthread_mutex_lock(&barrier->m);
	unsigned gen = barrier->gen;
	if (++barrier->num == barrier->count) {
		barrier->num = 0;
		barrier->gen++;
		pthread_cond_broadcast(&barrier->c);
	} else {
		while (barrier->num < barrier->count && barrier->gen == gen) {
			pthread_cond_wait(&barrier->c, &barrier->m);
		}
	}
	pthread_mutex_unlock(&barrier->m);
}

static void barrier_destroy(barrier_t* barrier) {
	pthread_mutex_destroy(&barrier->m);
	pthread_cond_destroy(&barrier->c);
}

/* Add to main scheduler. */
static void det_activate(unsigned id) {
	assert(active_count <= MAX_THREADS);
	sems_active[active_count++] = &sems[id];
}

/* Remove from main scheduler. */
static void det_deactivate(unsigned id) {
	assert(tls_sem);
	assert(active_count > 0);
	unsigned pos;
	for (pos = 0; pos < active_count; pos++) {
		if (sems_active[pos] == &sems[id])
		break;
	}
	assert(pos < active_count);
	memmove(&sems_active[pos], &sems_active[pos + 1],
                (active_count - pos - 1) * sizeof(sem_t*));
	active_count--;
}

bool det_active() {
	return tls_sem != NULL;
}

/* Remove from main scheduler, and add to mutex's waitlist.  Wait list
 * is *not* deterministic, but after waking, threads go through main
 * scheduler again.  Since jemalloc heavily uses mutexes, this
 * optimization is important to complete deterministic schedules in a
 * reasonable time, vs. the more straightforward
 *
 * while(true) {
 * 	det_before_shared()
 * 	ret = trylock();
 * 	det_after_shared();
 *      if (ret) break;
 * }
 *
 * *MUST HOLD* det_before_shared()
 * calls det_after_shared()
 */
void det_wait(waitlist_t** waiters) {
	if (!tls_sem) {
		assert(!started);
		return;
	}
	waitlist_t waitnode;

	sem_init(&waitnode.sem, 0, 0);
	det_deactivate(id);
	waitnode.id = id;
	waitnode.next = NULL;

	if (!*waiters) {
		*waiters = &waitnode;
	} else {
		waitlist_t* n = *waiters;
		while (n->next) {
			n = n->next;
		}
		n->next = &waitnode;
	}

	det_after_shared();
	sem_wait(&waitnode.sem);
	sem_destroy(&waitnode.sem);
}

/* Wake a thread from mutex's waitlist, and add it back
 * to the main scheduler.
 * *MUST HOLD* det_before_shared()
 */
void det_wake(waitlist_t** waiters) {
	assert(!started  || tls_sem);
	if (!tls_sem) {
		return;
	}

	if (!*waiters) {
		return;
	}

	waitlist_t *first = *waiters;
	*waiters = first->next;

	det_activate(first->id);
	sem_post(&first->sem);
}

/* Used before shared data access, it's a global *deterministic* lock,
 * based on initial seed.
 */
void det_before_shared() {
	if (tls_sem) {
		sem_wait(tls_sem);
	}
}

/* Schedule a single thread to run */
static void det_schedule_one() {
	assert(active_count != 0);
	unsigned long r = prng_lg_range_u64(&rand_state, 64);
	unsigned int pos = r % active_count;
	sem_post(sems_active[pos]);
}

/* Used after shared data access.  Wakes another thread
 * deterministically based on initial seed.  */
void det_after_shared() {
	if (!tls_sem) {
		return;
	}

	det_schedule_one();
}

/* Deactivate and cleanup this thread. */
static void det_thread_cleanup() {
	det_before_shared();
	det_deactivate(id);
	if (active_count > 0) {
		det_after_shared();
	}

	sem_destroy(tls_sem);
	tls_sem = NULL;
}

struct args {
	void* arg;
	unsigned id;
	void *(*proc)(void*);
};

/* New thread start routine, set up TLS vars. */
static void* det_thd_start(void* a) {
	struct args* args = a;

	id = args->id;

	void* arg = args->arg;
	void *(*proc)(void*) = args->proc;
	free(args);

	tls_sem = &sems[id];

	barrier_wait(&thread_barrier);
	/* starting is set, threads being added to scheduler */
	barrier_wait(&thread_barrier);

	void* res = proc(arg);

	/* Remove us from scheduler */
	det_thread_cleanup();

	barrier_wait(&thread_barrier);
	/* starting is unset, thread shutdown/join not tracked */
	barrier_wait(&thread_barrier);

	return res;
}

/* Add a new thread to this schedule. */
static int det_thd_create(pthread_t *thd, void *(*proc)(void *), void *arg) {
	struct args* args = malloc(sizeof(struct args));
	args->arg = arg;
	args->proc = proc;

	args->id = nextid++;
	int nid = args->id;
	if (args->id >= MAX_THREADS) {
		exit(-1);
	}
	sem_init(&sems[nid], 0, 0);

	pthread_create(thd, NULL, det_thd_start, args);

	return nid;
}

/* Start a schedule. Starts count threads via proc(arg) */
void det_schedule(unsigned seed, unsigned count,
                  void *(*proc)(void *), void *arg) {
	active_count = 0;
        barrier_init(&thread_barrier, count + 1);
	memset(sems_active, 0, sizeof(sems_active));
	nextid = 0;
	rand_state = seed;

	pthread_t thrs[count];

	for (unsigned i = 0; i < count; i++) {
		det_thd_create(&thrs[i], proc, arg);
	}

	barrier_wait(&thread_barrier);
	for (unsigned i = 0; i < count; i++) {
		det_activate(i);
	}
	started = true;
	det_schedule_one();
	barrier_wait(&thread_barrier);

	barrier_wait(&thread_barrier);
	started = false;
	barrier_wait(&thread_barrier);

	for(unsigned i = 0; i < count; i++) {
		void* ret;
		pthread_join(thrs[i], &ret);
	}

	barrier_destroy(&thread_barrier);
}

#endif /* JEMALLOC_DETERMINISTIC_SCHED */
