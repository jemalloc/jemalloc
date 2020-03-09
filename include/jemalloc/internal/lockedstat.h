#ifndef JEMALLOC_INTERNAL_LOCKEDSTAT_H
#define JEMALLOC_INTERNAL_LOCKEDSTAT_H

/*
 * In those architectures that support 64-bit atomics, we use atomic updates for
 * our 64-bit values.  Otherwise, we use a plain uint64_t and synchronize
 * externally.
 */

typedef struct lockedstat_u64_s lockedstat_u64_t;
#ifdef JEMALLOC_ATOMIC_U64
struct lockedstat_u64_s {
	atomic_u64_t val;
};
#else
/* Must hold the associated mutex. */
struct lockedstat_u64_s {
	uint64_t val;
};
#endif

typedef struct lockedstat_zu_s lockedstat_zu_t;
struct lockedstat_zu_s {
	atomic_zu_t val;
};

#ifndef JEMALLOC_ATOMIC_U64
#  define LOCKEDSTAT_MTX_DECLARE(name) malloc_mutex_t name;
#  define LOCKEDSTAT_MTX_INIT(ptr, name, rank, rank_mode)		\
    malloc_mutex_init(ptr, name, rank, rank_mode)
#  define LOCKEDSTAT_MTX(mtx) (&(mtx))
#  define LOCKEDSTAT_MTX_LOCK(tsdn, mu) malloc_mutex_lock(tsdn, &(mu))
#  define LOCKEDSTAT_MTX_UNLOCK(tsdn, mu) malloc_mutex_unlock(tsdn, &(mu))
#else
#  define LOCKEDSTAT_MTX_DECLARE(name)
#  define LOCKEDSTAT_MTX(ptr) NULL
#  define LOCKEDSTAT_MTX_INIT(ptr, name, rank, rank_mode) false
#  define LOCKEDSTAT_MTX_LOCK(tsdn, mu) do {} while (0)
#  define LOCKEDSTAT_MTX_UNLOCK(tsdn, mu) do {} while (0)
#endif

static inline uint64_t
lockedstat_read_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_u64_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_u64(&p->val, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	return p->val;
#endif
}

static inline void
lockedstat_inc_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_u64_t *p,
    uint64_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_u64(&p->val, x, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	p->val += x;
#endif
}

static inline void
lockedstat_dec_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_u64_t *p,
    uint64_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	uint64_t r = atomic_fetch_sub_u64(&p->val, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	p->val -= x;
	assert(p->val + x >= p->val);
#endif
}

/*
 * Non-atomically sets *dst += src.  *dst needs external synchronization.
 * This lets us avoid the cost of a fetch_add when its unnecessary (note that
 * the types here are atomic).
 */
static inline void
lockedstat_inc_u64_unsynchronized(lockedstat_u64_t *dst, uint64_t src) {
#ifdef JEMALLOC_ATOMIC_U64
	uint64_t cur_dst = atomic_load_u64(&dst->val, ATOMIC_RELAXED);
	atomic_store_u64(&dst->val, src + cur_dst, ATOMIC_RELAXED);
#else
	dst->val += src;
#endif
}

static inline uint64_t
lockedstat_read_u64_unsynchronized(lockedstat_u64_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_u64(&p->val, ATOMIC_RELAXED);
#else
	return p->val;
#endif

}

static inline size_t
lockedstat_read_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_zu_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
#endif
}

static inline void
lockedstat_inc_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_zu_t *p,
    size_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_zu(&p->val, x, ATOMIC_RELAXED);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	size_t cur = atomic_load_zu(&p->val, ATOMIC_RELAXED);
	atomic_store_zu(&p->val, cur + x, ATOMIC_RELAXED);
#endif
}

static inline void
lockedstat_dec_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, lockedstat_zu_t *p,
    size_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	size_t r = atomic_fetch_sub_zu(&p->val, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	malloc_mutex_assert_owner(tsdn, mtx);
	size_t cur = atomic_load_zu(&p->val, ATOMIC_RELAXED);
	atomic_store_zu(&p->val, cur - x, ATOMIC_RELAXED);
#endif
}

/* Like the _u64 variant, needs an externally synchronized *dst. */
static inline void
lockedstat_inc_zu_unsynchronized(lockedstat_zu_t *dst, size_t src) {
	size_t cur_dst = atomic_load_zu(&dst->val, ATOMIC_RELAXED);
	atomic_store_zu(&dst->val, src + cur_dst, ATOMIC_RELAXED);
}

/*
 * Unlike the _u64 variant, this is safe to call unconditionally.
 */
static inline size_t
lockedstat_read_atomic_zu(lockedstat_zu_t *p) {
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
}

#endif /* JEMALLOC_INTERNAL_LOCKEDSTAT_H */
