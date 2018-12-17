#include "jemalloc/internal/atomic.h"

#define FAST_MUTEX_INITIALIZER { ATOMIC_INIT(0) }

struct fast_mutex_s {
  atomic_u64_t word;
};

typedef struct fast_waiter_s fast_waiter_t;
struct fast_waiter_s;

struct fast_cond_s {
  pthread_mutex_t m;
  fast_waiter_t* w;
};

typedef struct fast_mutex_s fast_mutex_t;
typedef struct fast_cond_s fast_cond_t;

void fast_mutex_lock_slow(fast_mutex_t* m);
void fast_mutex_unlock_slow(fast_mutex_t* m);

static inline void fast_mutex_lock(fast_mutex_t* m) {
  uint64_t prev = 0;
  if (!atomic_compare_exchange_weak_u64(&m->word, &prev, 1, ATOMIC_ACQUIRE, ATOMIC_RELAXED)) {
    fast_mutex_lock_slow(m);
  }
}

static inline void fast_mutex_unlock(fast_mutex_t* m) {
  uint64_t prev = 1;
  if (!atomic_compare_exchange_strong_u64(&m->word, &prev, 0, ATOMIC_RELEASE, ATOMIC_RELAXED)) {
    assert((prev& 1) == 1);
    fast_mutex_unlock_slow(m);
  }
}

static inline bool fast_mutex_trylock(fast_mutex_t* m) {
  uint64_t prev = 0;
  if (!atomic_compare_exchange_weak_u64(&m->word, &prev, 1, ATOMIC_ACQUIRE, ATOMIC_RELAXED)) {
    return true;
  }
  return false;
}

static inline bool fast_mutex_locked(fast_mutex_t* m) {
  return (atomic_load_u64(&m->word, ATOMIC_RELAXED) & 1) != 0;
}

static inline void fast_mutex_init(fast_mutex_t* m) {
  atomic_store_u64(&m->word, 0, ATOMIC_RELAXED);
}

static inline int fast_cond_init(fast_cond_t* c, void* unused) {
  int ret;
  ret = pthread_mutex_init(&c->m, NULL);
  if (ret) {
    return ret;
  }
  c->w = NULL;
  
  return 0;
}

int fast_cond_wait(fast_cond_t* c, fast_mutex_t* m);
int fast_cond_timedwait(fast_cond_t* c, fast_mutex_t* m, struct timespec*ts);
void fast_cond_signal(fast_cond_t* c);

