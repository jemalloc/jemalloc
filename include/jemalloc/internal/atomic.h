#ifndef JEMALLOC_INTERNAL_ATOMIC_H
#define JEMALLOC_INTERNAL_ATOMIC_H

#define ATOMIC_INLINE static inline

#if defined(JEMALLOC_GCC_ATOMIC_ATOMICS)
#  include "jemalloc/internal/atomic_gcc_atomic.h"
#elif defined(JEMALLOC_GCC_SYNC_ATOMICS)
#  include "jemalloc/internal/atomic_gcc_sync.h"
#elif defined(_MSC_VER)
#  include "jemalloc/internal/atomic_msvc.h"
#elif defined(JEMALLOC_C11_ATOMICS)
#  include "jemalloc/internal/atomic_c11.h"
#else
#  error "Don't have atomics implemented on this platform."
#endif

/*
 * This header gives more or less a backport of C11 atomics. The user can write
 * JEMALLOC_GENERATE_ATOMICS(type, short_type, lg_sizeof_type); to generate
 * counterparts of the C11 atomic functions for type, as so:
 *   JEMALLOC_GENERATE_ATOMICS(int *, pi, 3);
 * and then write things like:
 *   int *some_ptr;
 *   atomic_pi_t atomic_ptr_to_int;
 *   atomic_store_pi(&atomic_ptr_to_int, some_ptr, ATOMIC_RELAXED);
 *   int *prev_value = atomic_exchange_pi(&ptr_to_int, NULL, ATOMIC_ACQ_REL);
 *   assert(some_ptr == prev_value);
 * and expect things to work in the obvious way.
 *
 * Also included (with naming differences to avoid conflicts with the standard
 * library):
 *   atomic_fence(atomic_memory_order_t) (mimics C11's atomic_thread_fence).
 *   ATOMIC_INIT (mimics C11's ATOMIC_VAR_INIT).
 */

/*
 * Pure convenience, so that we don't have to type "atomic_memory_order_"
 * quite so often.
 */
#define ATOMIC_RELAXED atomic_memory_order_relaxed
#define ATOMIC_ACQUIRE atomic_memory_order_acquire
#define ATOMIC_RELEASE atomic_memory_order_release
#define ATOMIC_ACQ_REL atomic_memory_order_acq_rel
#define ATOMIC_SEQ_CST atomic_memory_order_seq_cst

/*
 * Not all platforms have 64-bit atomics.  If we do, this #define exposes that
 * fact.
 */
#if (LG_SIZEOF_PTR == 3 || LG_SIZEOF_INT == 3)
#  define JEMALLOC_ATOMIC_U64
#endif

/*
 * In order to let us transition atomics usage piecemeal (and reason locally
 * about memory orders), we'll support the previous API for a while.
 */
#define JEMALLOC_GENERATE_COMPATABILITY_ATOMICS(type, short_type)	\
ATOMIC_INLINE type							\
atomic_read_##short_type(type *p) {					\
	return atomic_load_##short_type ((atomic_##short_type##_t *)p,	\
	    ATOMIC_SEQ_CST);						\
}									\
									\
ATOMIC_INLINE void							\
atomic_write_##short_type(type *p, const type val) {			\
	atomic_store_##short_type((atomic_##short_type##_t *)p,		\
	    (type)val, ATOMIC_SEQ_CST);					\
}									\
ATOMIC_INLINE bool							\
atomic_cas_##short_type(type *p, type c, type s) {			\
	/* Note the '!' -- atomic_cas inverts the usual semantics. */	\
	return !atomic_compare_exchange_strong_##short_type(		\
	    (atomic_##short_type##_t *)p, &c, s, ATOMIC_SEQ_CST,	\
	    ATOMIC_SEQ_CST);						\
}

#define JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(type, short_type)	\
JEMALLOC_GENERATE_COMPATABILITY_ATOMICS(type, short_type)		\
									\
ATOMIC_INLINE type							\
atomic_add_##short_type(type *p, type x) {				\
	return atomic_fetch_add_##short_type(				\
	    (atomic_##short_type##_t *)p, x, ATOMIC_SEQ_CST) + x;	\
}									\
ATOMIC_INLINE type							\
atomic_sub_##short_type(type *p, type x) {				\
	return atomic_fetch_sub_##short_type(				\
	    (atomic_##short_type##_t *)p, x, ATOMIC_SEQ_CST) - x;	\
}

JEMALLOC_GENERATE_ATOMICS(void *, p, LG_SIZEOF_PTR)
JEMALLOC_GENERATE_COMPATABILITY_ATOMICS(void *, p)

/*
 * There's no actual guarantee that sizeof(bool) == 1, but it's true on the only
 * platform that actually needs to know the size, MSVC.
 */
JEMALLOC_GENERATE_ATOMICS(bool, b, 0)
JEMALLOC_GENERATE_COMPATABILITY_ATOMICS(bool, b)

JEMALLOC_GENERATE_INT_ATOMICS(unsigned, u, LG_SIZEOF_INT)
JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(unsigned, u)

JEMALLOC_GENERATE_INT_ATOMICS(size_t, zu, LG_SIZEOF_PTR)
JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(size_t, zu)

JEMALLOC_GENERATE_INT_ATOMICS(ssize_t, zd, LG_SIZEOF_PTR)
JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(ssize_t, zd)

JEMALLOC_GENERATE_INT_ATOMICS(uint32_t, u32, 2)
JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(uint32_t, u32)

#ifdef JEMALLOC_ATOMIC_U64
JEMALLOC_GENERATE_INT_ATOMICS(uint64_t, u64, 3)
JEMALLOC_GENERATE_COMPATABILITY_INT_ATOMICS(uint64_t, u64)
#endif

#undef ATOMIC_INLINE

#endif /* JEMALLOC_INTERNAL_ATOMIC_H */
