#ifdef JEMALLOC_INTERNAL_TSD_INTERNALS_H
#error This file should be included only once, by one of tsd_malloc_thread_cleanup.h, tsd_tls.h, tsd_generic.h, or tsd_win.h
#endif
#define JEMALLOC_INTERNAL_TSD_INTERNALS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena_decay_constants.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/tsd_binshards.h"
#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/peak.h"
#include "jemalloc/internal/rtree_tsd.h"
#include "jemalloc/internal/tcache.h"
#include "jemalloc/internal/thread_event_registry.h"
#include "jemalloc/internal/tsd_types.h"
#include "jemalloc/internal/util.h"
#include "jemalloc/internal/witness.h"

/*
 * Forward decls.  tsd_internals.h cannot include arena.h / prof.h directly:
 * those headers' STRUCTS-section includes trigger mutex.h -> tsd.h ->
 * tsd_generic.h, which would re-enter this file before its body finishes.
 * Each consumer here only uses these as pointer types.
 */
typedef struct arena_s      arena_t;
typedef struct prof_tdata_s prof_tdata_t;

/*
 * JEMALLOC_TLS_ADDR(tlsvar): take a thread-local's address so the compiler
 * cannot cache it across a user-space context switch.  A raw `&tlsvar` is
 * `thread_pointer + const_offset`, loop-invariant; inlined into malloc/free
 * under LTO it can be hoisted across a swapcontext and reused on the OS thread a
 * fiber migrated to -- a stale tsd/tcache.  Route all tsd access through this
 * macro; never take `&tsd_tls` raw.  See
 * https://github.com/jemalloc/jemalloc/issues/2890
 *
 * Static-TLS fast path (gated on JEMALLOC_TLS_MODEL_INITIAL_EXEC): re-read the
 * thread pointer with a volatile asm and add a runtime-captured constant offset.
 * The offset MUST be captured by the noinline helper, not computed inline as
 * `&tlsvar - thread_pointer` -- inline, the two terms hoist independently and
 * cancel the volatile read back to the stale address.  The offset is read and
 * written with relaxed atomics -- threads racing their first allocation init it
 * concurrently with the same thread-independent value.  Other configs use a
 * noinline `memory`-barrier accessor.  MSVC has no inline asm, so it keeps the
 * plain address; an MSVC build whose fibers run under whole-program opt (/GL)
 * must instead compile with /GT (fiber-safe TLS).
 *
 * DECLARE is emitted in every TU; DEFINE (the out-of-line bodies) only under
 * JEMALLOC_TSD_C_, i.e. once in src/tsd.c.  Both must be invoked directly, not
 * forwarded through another macro, or `##tlsvar` pastes the macro-expanded
 * `je_tsd_tls` instead of the literal name.
 */
#if defined(__GNUC__) && !defined(_WIN32) &&                                   \
    defined(JEMALLOC_TLS_MODEL_INITIAL_EXEC) &&                                \
    (defined(__aarch64__) || defined(__arm__) || defined(__x86_64__) ||        \
    defined(__i386__))
JEMALLOC_ALWAYS_INLINE char *
jemalloc_thread_pointer(void) {
	char *thread_pointer;
#  if defined(__aarch64__) && defined(__APPLE__)
	__asm__ __volatile__("mrs %0, tpidrro_el0\n\tbic %0, %0, #7" : "=r"(thread_pointer));
#  elif defined(__aarch64__)
	__asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(thread_pointer));
#  elif defined(__arm__)
	__asm__ __volatile__("mrc p15, 0, %0, c13, c0, 3\n\tbic %0, %0, #3" : "=r"(thread_pointer));
#  elif defined(__x86_64__) && defined(__APPLE__)
	__asm__ __volatile__("movq %%gs:0, %0" : "=r"(thread_pointer));
#  elif defined(__x86_64__)
	__asm__ __volatile__("movq %%fs:0, %0" : "=r"(thread_pointer));
#  else /* __i386__ */
	__asm__ __volatile__("movl %%gs:0, %0" : "=r"(thread_pointer));
#  endif
	return thread_pointer;
}
/* 1 is unreachable: tlsvar and the thread pointer are at least 4-aligned. */
#  define JEMALLOC_TLS_OFFSET_UNINITIALIZED 1
#  define JEMALLOC_TLS_ADDR_DECLARE(tlsvar)                                    \
	extern intptr_t jemalloc_tls_offset_##tlsvar;                                 \
	intptr_t jemalloc_tls_offset_init_##tlsvar(void);                             \
	JEMALLOC_ALWAYS_INLINE __typeof__(&(tlsvar))                                  \
	jemalloc_tls_addr_##tlsvar(void) {                                            \
		intptr_t tls_offset = __atomic_load_n(&jemalloc_tls_offset_##tlsvar,         \
		    __ATOMIC_RELAXED);                                                       \
		if (unlikely(tls_offset == JEMALLOC_TLS_OFFSET_UNINITIALIZED)) {             \
			tls_offset = jemalloc_tls_offset_init_##tlsvar();                           \
		}                                                                            \
		return (__typeof__(&(tlsvar)))(jemalloc_thread_pointer() +                   \
		    tls_offset);                                                             \
	}
#  define JEMALLOC_TLS_ADDR_DEFINE(tlsvar)                                     \
	intptr_t jemalloc_tls_offset_##tlsvar =                                       \
	    JEMALLOC_TLS_OFFSET_UNINITIALIZED;                                        \
	JEMALLOC_NOINLINE intptr_t                                                    \
	jemalloc_tls_offset_init_##tlsvar(void) {                                     \
		intptr_t tls_offset = (intptr_t)((char *)&(tlsvar) -                         \
		    jemalloc_thread_pointer());                                              \
		__atomic_store_n(&jemalloc_tls_offset_##tlsvar, tls_offset,                  \
		    __ATOMIC_RELAXED);                                                       \
		return tls_offset;                                                           \
	}
#  define JEMALLOC_TLS_ADDR(tlsvar) (jemalloc_tls_addr_##tlsvar())
#elif defined(__GNUC__)
#  define JEMALLOC_TLS_ADDR_DECLARE(tlsvar)                                    \
	__typeof__(&(tlsvar)) jemalloc_tls_addr_##tlsvar(void);
#  define JEMALLOC_TLS_ADDR_DEFINE(tlsvar)                                     \
	JEMALLOC_NOINLINE __typeof__(&(tlsvar))                                       \
	jemalloc_tls_addr_##tlsvar(void) {                                            \
		__typeof__(&(tlsvar)) tls_addr = &(tlsvar);                                  \
		__asm__ __volatile__("" : "+r"(tls_addr) : : "memory");                      \
		return tls_addr;                                                             \
	}
#  define JEMALLOC_TLS_ADDR(tlsvar) (jemalloc_tls_addr_##tlsvar())
#else
#  define JEMALLOC_TLS_ADDR_DECLARE(tlsvar)
#  define JEMALLOC_TLS_ADDR_DEFINE(tlsvar)
#  define JEMALLOC_TLS_ADDR(tlsvar) (&(tlsvar))
#endif

/*
 * Thread-Specific-Data layout
 *
 * At least some thread-local data gets touched on the fast-path of almost all
 * malloc operations.  But much of it is only necessary down slow-paths, or
 * testing.  We want to colocate the fast-path data so that it can live on the
 * same cacheline if possible.  So we define three tiers of hotness:
 * TSD_DATA_FAST: Touched on the alloc/dalloc fast paths.
 * TSD_DATA_SLOW: Touched down slow paths.  "Slow" here is sort of general;
 *     there are "semi-slow" paths like "not a sized deallocation, but can still
 *     live in the tcache".  We'll want to keep these closer to the fast-path
 *     data.
 * TSD_DATA_SLOWER: Only touched in test or debug modes, or not touched at all.
 *
 * An additional concern is that the larger tcache bins won't be used (we have a
 * bin per size class, but by default only cache relatively small objects).  So
 * the earlier bins are in the TSD_DATA_FAST tier, but the later ones are in the
 * TSD_DATA_SLOWER tier.
 *
 * As a result of all this, we put the slow data first, then the fast data, then
 * the slower data, while keeping the tcache as the last element of the fast
 * data (so that the fast -> slower transition happens midway through the
 * tcache).  While we don't yet play alignment tricks to guarantee it, this
 * increases our odds of getting some cache/page locality on fast paths.
 */

#ifdef JEMALLOC_JET
typedef void (*test_callback_t)(int *);
#	define MALLOC_TSD_TEST_DATA_INIT 0x72b65c10
#	define MALLOC_TEST_TSD                                                \
		O(test_data, int, int)                                         \
		O(test_callback, test_callback_t, int)
#	define MALLOC_TEST_TSD_INITIALIZER , MALLOC_TSD_TEST_DATA_INIT, NULL
#else
#	define MALLOC_TEST_TSD
#	define MALLOC_TEST_TSD_INITIALIZER
#endif

/*  O(name,			type,			nullable type) */
#define TSD_DATA_SLOW                                                          \
	O(tcache_enabled, bool, bool)                                          \
	O(reentrancy_level, int8_t, int8_t)                                    \
	O(min_init_state_nfetched, uint8_t, uint8_t)                           \
	O(thread_allocated_last_event, uint64_t, uint64_t)                     \
	O(thread_allocated_next_event, uint64_t, uint64_t)                     \
	O(thread_deallocated_last_event, uint64_t, uint64_t)                   \
	O(thread_deallocated_next_event, uint64_t, uint64_t)                   \
	O(te_data, te_data_t, te_data_t)                                       \
	O(prof_sample_last_event, uint64_t, uint64_t)                          \
	O(stats_interval_last_event, uint64_t, uint64_t)                       \
	O(prof_tdata, prof_tdata_t *, prof_tdata_t *)                          \
	O(prng_state, uint64_t, uint64_t)                                      \
	O(san_extents_until_guard_small, uint64_t, uint64_t)                   \
	O(san_extents_until_guard_large, uint64_t, uint64_t)                   \
	O(iarena, arena_t *, arena_t *)                                        \
	O(arena, arena_t *, arena_t *)                                         \
	O(arena_decay_ticker, ticker_geom_t, ticker_geom_t)                    \
	O(sec_shard, uint8_t, uint8_t)                                         \
	O(binshards, tsd_binshards_t, tsd_binshards_t)                         \
	O(peak, peak_t, peak_t)                                                \
	O(tcache_slow, tcache_slow_t, tcache_slow_t)                           \
	O(rtree_ctx, rtree_ctx_t, rtree_ctx_t)

#define TSD_DATA_SLOW_INITIALIZER                                              \
	/* tcache_enabled */ TCACHE_ENABLED_ZERO_INITIALIZER,                  \
	    /* reentrancy_level */ 0, /* min_init_state_nfetched */ 0,         \
	    /* thread_allocated_last_event */ 0,                               \
	    /* thread_allocated_next_event */ 0,                               \
	    /* thread_deallocated_last_event */ 0,                             \
	    /* thread_deallocated_next_event */ 0,                             \
	    /* te_data */ TE_DATA_INITIALIZER, /* prof_sample_last_event */ 0, \
	    /* stats_interval_last_event */ 0, /* prof_tdata */ NULL,          \
	    /* prng_state */ 0, /* san_extents_until_guard_small */ 0,         \
	    /* san_extents_until_guard_large */ 0, /* iarena */ NULL,          \
	    /* arena */ NULL, /* arena_decay_ticker */                         \
	    TICKER_GEOM_INIT(ARENA_DECAY_NTICKS_PER_UPDATE),                   \
	    /* sec_shard */ (uint8_t) - 1,                                     \
	    /* binshards */ TSD_BINSHARDS_ZERO_INITIALIZER,                    \
	    /* peak */ PEAK_INITIALIZER, /* tcache_slow */                     \
	    TCACHE_SLOW_ZERO_INITIALIZER,                                      \
	    /* rtree_ctx */ RTREE_CTX_INITIALIZER,

/*  O(name,			type,			nullable type) */
#define TSD_DATA_FAST                                                          \
	O(thread_allocated, uint64_t, uint64_t)                                \
	O(thread_allocated_next_event_fast, uint64_t, uint64_t)                \
	O(thread_deallocated, uint64_t, uint64_t)                              \
	O(thread_deallocated_next_event_fast, uint64_t, uint64_t)              \
	O(tcache, tcache_t, tcache_t)

#define TSD_DATA_FAST_INITIALIZER                                              \
	/* thread_allocated */ 0, /* thread_allocated_next_event_fast */ 0,    \
	    /* thread_deallocated */ 0,                                        \
	    /* thread_deallocated_next_event_fast */ 0,                        \
	    /* tcache */ TCACHE_ZERO_INITIALIZER,

/*  O(name,			type,			nullable type) */
#define TSD_DATA_SLOWER                                                        \
	O(witness_tsd, witness_tsd_t, witness_tsdn_t)                          \
	MALLOC_TEST_TSD

#define TSD_DATA_SLOWER_INITIALIZER                                            \
	/* witness */ WITNESS_TSD_INITIALIZER                                  \
	/* test data */ MALLOC_TEST_TSD_INITIALIZER

#define TSD_INITIALIZER                                                        \
	{                                                                      \
		TSD_DATA_SLOW_INITIALIZER                                      \
		/* state */ tsd_state_uninitialized,                           \
		    TSD_DATA_FAST_INITIALIZER TSD_DATA_SLOWER_INITIALIZER      \
	}

#if defined(JEMALLOC_MALLOC_THREAD_CLEANUP) || defined(_WIN32)
void _malloc_tsd_cleanup_register(bool (*f)(void));
#endif

void  *malloc_tsd_malloc(size_t size);
void   malloc_tsd_dalloc(void *wrapper);
tsd_t *malloc_tsd_boot0(void);
void   malloc_tsd_boot1(void);
void   tsd_cleanup(void *arg);
tsd_t *tsd_fetch_slow(tsd_t *tsd, bool minimal);
void   tsd_state_set(tsd_t *tsd, uint8_t new_state);
void   tsd_slow_update(tsd_t *tsd);

#define TSD_MIN_INIT_STATE_MAX_FETCHED (128)

enum {
	/* Common case --> jnz. */
	tsd_state_nominal = 0,
	/* Initialized but on slow path. */
	tsd_state_nominal_slow = 1,
	/*
	 * The above nominal states should be lower values.  We use
	 * tsd_nominal_max to separate nominal states from threads in the
	 * process of being born / dying.
	 */
	tsd_state_nominal_max = 1,

	/*
	 * A thread might free() during its death as its only allocator action;
	 * in such scenarios, we need tsd, but set up in such a way that no
	 * cleanup is necessary.
	 */
	tsd_state_minimal_initialized = 2,
	/* States during which we know we're in thread death. */
	tsd_state_purgatory = 3,
	tsd_state_reincarnated = 4,
	/*
	 * What it says on the tin; tsd that hasn't been initialized.  Note
	 * that even when the tsd struct lives in TLS, when need to keep track
	 * of stuff like whether or not our pthread destructors have been
	 * scheduled, so this really truly is different than the nominal state.
	 */
	tsd_state_uninitialized = 5
};

/*
 * Some TSD accesses can only be done in a nominal state.  To enforce this, we
 * wrap TSD member access in a function that asserts on TSD state, and mangle
 * field names to prevent touching them accidentally.
 */
#define TSD_MANGLE(n) cant_access_tsd_items_directly_use_a_getter_or_setter_##n

/* The actual tsd. */
struct tsd_s {
	/*
	 * The contents should be treated as totally opaque outside the tsd
	 * module.  Access any thread-local state through the getters and
	 * setters below.
	 */

#define O(n, t, nt) t TSD_MANGLE(n);

	TSD_DATA_SLOW
	/* Encodes one of tsd_state_*; mutated only by the owning thread. */
	uint8_t state;
	TSD_DATA_FAST
	TSD_DATA_SLOWER
#undef O
};

JEMALLOC_ALWAYS_INLINE uint8_t
tsd_state_get(tsd_t *tsd) {
	return tsd->state;
}

/*
 * Wrapper around tsd_t that makes it possible to avoid implicit conversion
 * between tsd_t and tsdn_t, where tsdn_t is "nullable" and has to be
 * explicitly converted to tsd_t, which is non-nullable.
 */
struct tsdn_s {
	tsd_t tsd;
};
#define TSDN_NULL ((tsdn_t *)0)
JEMALLOC_ALWAYS_INLINE tsdn_t *
tsd_tsdn(tsd_t *tsd) {
	return (tsdn_t *)tsd;
}

JEMALLOC_ALWAYS_INLINE bool
tsdn_null(const tsdn_t *tsdn) {
	return tsdn == NULL;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsdn_tsd(tsdn_t *tsdn) {
	assert(!tsdn_null(tsdn));

	return &tsdn->tsd;
}
