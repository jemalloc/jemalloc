#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/fxp.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_init.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/thread_event.h"
#include "jemalloc/internal/util.h"

#include "jemalloc/internal/conf.h"

/******************************************************************************/
/* Data. */

/* Runtime configuration options. */
const char *je_malloc_conf
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
        ;
/*
 * The usual rule is that the closer to runtime you are, the higher priority
 * your configuration settings are (so the jemalloc config options get lower
 * priority than the per-binary setting, which gets lower priority than the /etc
 * setting, which gets lower priority than the environment settings).
 *
 * But it's a fairly common use case in some testing environments for a user to
 * be able to control the binary, but nothing else (e.g. a performance canary
 * uses the production OS and environment variables, but can run any binary in
 * those circumstances).  For these use cases, it's handy to have an in-binary
 * mechanism for overriding environment variable settings, with the idea that if
 * the results are positive they get promoted to the official settings, and
 * moved from the binary to the environment variable.
 *
 * We don't actually want this to be widespread, so we'll give it a silly name
 * and not mention it in headers or documentation.
 */
const char *je_malloc_conf_2_conf_harder
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
        ;

const char *opt_malloc_conf_symlink = NULL;
const char *opt_malloc_conf_env_var = NULL;

bool opt_abort =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
bool opt_abort_conf =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
/* Intentionally default off, even with debug builds. */
bool        opt_confirm_conf = false;
const char *opt_junk =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    "true"
#else
    "false"
#endif
    ;
bool opt_junk_alloc =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;
bool opt_junk_free =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;
bool opt_trust_madvise =
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
    false
#else
    true
#endif
    ;

bool opt_cache_oblivious =
#ifdef JEMALLOC_CACHE_OBLIVIOUS
    true
#else
    false
#endif
    ;

zero_realloc_action_t opt_zero_realloc_action =
#ifdef JEMALLOC_ZERO_REALLOC_DEFAULT_FREE
    zero_realloc_action_free
#else
    zero_realloc_action_alloc
#endif
    ;

atomic_zu_t zero_realloc_count = ATOMIC_INIT(0);

/*
 * Disable large size classes is now the default behavior in jemalloc.
 * Although it is configurable in MALLOC_CONF, this is mainly for debugging
 * purposes and should not be tuned.
 */
bool opt_disable_large_size_classes = true;

const char *const zero_realloc_mode_names[] = {
    "alloc",
    "free",
    "abort",
};

/*
 * These are the documented values for junk fill debugging facilities -- see the
 * man page.
 */
static const uint8_t junk_alloc_byte = 0xa5;
static const uint8_t junk_free_byte = 0x5a;

static void
default_junk_alloc(void *ptr, size_t usize) {
	memset(ptr, junk_alloc_byte, usize);
}

static void
default_junk_free(void *ptr, size_t usize) {
	memset(ptr, junk_free_byte, usize);
}

void (*JET_MUTABLE junk_alloc_callback)(
    void *ptr, size_t size) = &default_junk_alloc;
void (*JET_MUTABLE junk_free_callback)(
    void *ptr, size_t size) = &default_junk_free;
void (*JET_MUTABLE invalid_conf_abort)(void) = &abort;

bool         opt_utrace = false;
bool         opt_xmalloc = false;
bool         opt_experimental_infallible_new = false;
bool         opt_experimental_tcache_gc = true;
bool         opt_zero = false;
unsigned     opt_narenas = 0;
fxp_t opt_narenas_ratio = FXP_INIT_INT(4);

unsigned ncpus;

unsigned opt_debug_double_free_max_scan =
    SAFETY_CHECK_DOUBLE_FREE_MAX_SCAN_DEFAULT;

size_t opt_calloc_madvise_threshold = CALLOC_MADVISE_THRESHOLD_DEFAULT;

/* The global hpa, and whether it's on. */
bool             opt_hpa = false;
hpa_shard_opts_t opt_hpa_opts = HPA_SHARD_OPTS_DEFAULT;
sec_opts_t       opt_hpa_sec_opts = SEC_OPTS_DEFAULT;

/* False should be the common case.  Set to true to trigger initialization. */
bool malloc_slow = true;

typedef struct {
	void  *p; /* Input pointer (as in realloc(p, s)). */
	size_t s; /* Request size. */
	void  *r; /* Result pointer. */
} malloc_utrace_t;

#ifdef JEMALLOC_UTRACE
#	define UTRACE(a, b, c)                                                \
		do {                                                           \
			if (unlikely(opt_utrace)) {                            \
				int             utrace_serrno = errno;         \
				malloc_utrace_t ut;                            \
				ut.p = (a);                                    \
				ut.s = (b);                                    \
				ut.r = (c);                                    \
				UTRACE_CALL(&ut, sizeof(ut));                  \
				errno = utrace_serrno;                         \
			}                                                      \
		} while (0)
#else
#	define UTRACE(a, b, c)
#endif


/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

/*
 * FreeBSD's libc uses the bootstrap_*() functions in bootstrap-sensitive
 * situations that cannot tolerate TLS variable access (TLS allocation and very
 * early internal data structure initialization).
 */

void *
bootstrap_malloc(size_t size) {
	if (unlikely(size == 0)) {
		size = 1;
	}

	return a0ialloc(size, false, false);
}

void *
bootstrap_calloc(size_t num, size_t size) {
	size_t num_size;

	num_size = num * size;
	if (unlikely(num_size == 0)) {
		assert(num == 0 || size == 0);
		num_size = 1;
	}

	return a0ialloc(num_size, true, false);
}

void
bootstrap_free(void *ptr) {
	if (unlikely(ptr == NULL)) {
		return;
	}

	a0idalloc(ptr, false);
}

/*
 * Ensure that we don't hold any locks upon entry to or exit from allocator
 * code (in a "broad" sense that doesn't count a reentrant allocation as an
 * entrance or exit).
 */
JEMALLOC_ALWAYS_INLINE void
check_entry_exit_locking(tsdn_t *tsdn) {
	if (!config_debug) {
		return;
	}
	if (tsdn_null(tsdn)) {
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	/*
	 * It's possible we hold locks at entry/exit if we're in a nested
	 * allocation.
	 */
	int8_t reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (reentrancy_level != 0) {
		return;
	}
	witness_assert_lockless(tsdn_witness_tsdp_get(tsdn));
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin allocation-path internal functions and data structures.
 */

/*
 * Settings determined by the documented behavior of the allocation functions.
 */
typedef struct static_opts_s static_opts_t;
struct static_opts_s {
	/* Whether or not allocation size may overflow. */
	bool may_overflow;

	/*
	 * Whether or not allocations (with alignment) of size 0 should be
	 * treated as size 1.
	 */
	bool bump_empty_aligned_alloc;
	/*
	 * Whether to assert that allocations are not of size 0 (after any
	 * bumping).
	 */
	bool assert_nonempty_alloc;

	/*
	 * Whether or not to modify the 'result' argument to malloc in case of
	 * error.
	 */
	bool null_out_result_on_error;
	/* Whether to set errno when we encounter an error condition. */
	bool set_errno_on_error;

	/*
	 * The minimum valid alignment for functions requesting aligned storage.
	 */
	size_t min_alignment;

	/* The error string to use if we oom. */
	const char *oom_string;
	/* The error string to use if the passed-in alignment is invalid. */
	const char *invalid_alignment_string;

	/*
	 * False if we're configured to skip some time-consuming operations.
	 *
	 * This isn't really a malloc "behavior", but it acts as a useful
	 * summary of several other static (or at least, static after program
	 * initialization) options.
	 */
	bool slow;
	/*
	 * Return size.
	 */
	bool usize;
};

JEMALLOC_ALWAYS_INLINE void
static_opts_init(static_opts_t *static_opts) {
	static_opts->may_overflow = false;
	static_opts->bump_empty_aligned_alloc = false;
	static_opts->assert_nonempty_alloc = false;
	static_opts->null_out_result_on_error = false;
	static_opts->set_errno_on_error = false;
	static_opts->min_alignment = 0;
	static_opts->oom_string = "";
	static_opts->invalid_alignment_string = "";
	static_opts->slow = false;
	static_opts->usize = false;
}

typedef struct dynamic_opts_s dynamic_opts_t;
struct dynamic_opts_s {
	void   **result;
	size_t   usize;
	size_t   num_items;
	size_t   item_size;
	size_t   alignment;
	bool     zero;
	unsigned tcache_ind;
	unsigned arena_ind;
};

JEMALLOC_ALWAYS_INLINE void
dynamic_opts_init(dynamic_opts_t *dynamic_opts) {
	dynamic_opts->result = NULL;
	dynamic_opts->usize = 0;
	dynamic_opts->num_items = 0;
	dynamic_opts->item_size = 0;
	dynamic_opts->alignment = 0;
	dynamic_opts->zero = false;
	dynamic_opts->tcache_ind = TCACHE_IND_AUTOMATIC;
	dynamic_opts->arena_ind = ARENA_IND_AUTOMATIC;
}

/*
 * ind parameter is optional and is only checked and filled if alignment == 0;
 * return true if result is out of range.
 */
JEMALLOC_ALWAYS_INLINE bool
aligned_usize_get(size_t size, size_t alignment, size_t *usize, szind_t *ind,
    bool bump_empty_aligned_alloc) {
	assert(usize != NULL);
	if (alignment == 0) {
		if (ind != NULL) {
			*ind = sz_size2index(size);
			if (unlikely(*ind >= SC_NSIZES)) {
				return true;
			}
			*usize = sz_large_size_classes_disabled()
			    ? sz_s2u(size)
			    : sz_index2size(*ind);
			assert(*usize > 0 && *usize <= SC_LARGE_MAXCLASS);
			return false;
		}
		*usize = sz_s2u(size);
	} else {
		if (bump_empty_aligned_alloc && unlikely(size == 0)) {
			size = 1;
		}
		*usize = sz_sa2u(size, alignment);
	}
	if (unlikely(*usize == 0 || *usize > SC_LARGE_MAXCLASS)) {
		return true;
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
zero_get(bool guarantee, bool slow) {
	if (config_fill && slow && unlikely(opt_zero)) {
		return true;
	} else {
		return guarantee;
	}
}

/* Return true if a manual arena is specified and arena_get() OOMs. */
JEMALLOC_ALWAYS_INLINE bool
arena_get_from_ind(tsd_t *tsd, unsigned arena_ind, arena_t **arena_p) {
	if (arena_ind == ARENA_IND_AUTOMATIC) {
		/*
		 * In case of automatic arena management, we defer arena
		 * computation until as late as we can, hoping to fill the
		 * allocation out of the tcache.
		 */
		*arena_p = NULL;
	} else {
		*arena_p = arena_get(tsd_tsdn(tsd), arena_ind, true);
		if (unlikely(*arena_p == NULL) && arena_ind >= narenas_auto) {
			return true;
		}
	}
	return false;
}

/* ind is ignored if dopts->alignment > 0. */
JEMALLOC_ALWAYS_INLINE void *
imalloc_no_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t size, size_t usize, szind_t ind, bool slab) {
	/* Fill in the tcache. */
	tcache_t *tcache = tcache_get_from_ind(
	    tsd, dopts->tcache_ind, sopts->slow, /* is_alloc */ true);

	/* Fill in the arena. */
	arena_t *arena;
	if (arena_get_from_ind(tsd, dopts->arena_ind, &arena)) {
		return NULL;
	}

	if (unlikely(dopts->alignment != 0)) {
		return ipalloct_explicit_slab(tsd_tsdn(tsd), usize,
		    dopts->alignment, dopts->zero, slab, tcache, arena);
	}

	return iallocztm_explicit_slab(tsd_tsdn(tsd), size, ind, dopts->zero,
	    slab, tcache, false, arena, sopts->slow);
}

JEMALLOC_ALWAYS_INLINE void *
imalloc_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t usize, szind_t ind) {
	void *ret;

	dopts->alignment = prof_sample_align(usize, dopts->alignment);
	/*
	 * If the allocation is small enough that it would normally be allocated
	 * on a slab, we need to take additional steps to ensure that it gets
	 * its own extent instead.
	 */
	if (sz_can_use_slab(usize)) {
		assert((dopts->alignment & PROF_SAMPLE_ALIGNMENT_MASK) == 0);
		size_t  bumped_usize = sz_sa2u(usize, dopts->alignment);
		szind_t bumped_ind = sz_size2index(bumped_usize);
		dopts->tcache_ind = TCACHE_IND_NONE;
		ret = imalloc_no_sample(sopts, dopts, tsd, bumped_usize,
		    bumped_usize, bumped_ind, /* slab */ false);
		if (unlikely(ret == NULL)) {
			return NULL;
		}
		arena_prof_promote(tsd_tsdn(tsd), ret, usize, bumped_usize);
	} else {
		ret = imalloc_no_sample(sopts, dopts, tsd, usize, usize, ind,
		    /* slab */ false);
	}
	assert(prof_sample_aligned(ret));

	return ret;
}

/*
 * Returns true if the allocation will overflow, and false otherwise.  Sets
 * *size to the product either way.
 */
JEMALLOC_ALWAYS_INLINE bool
compute_size_with_overflow(
    bool may_overflow, dynamic_opts_t *dopts, size_t *size) {
	/*
	 * This function is just num_items * item_size, except that we may have
	 * to check for overflow.
	 */

	if (!may_overflow) {
		assert(dopts->num_items == 1);
		*size = dopts->item_size;
		return false;
	}

	/* A size_t with its high-half bits all set to 1. */
	static const size_t high_bits = SIZE_T_MAX << (sizeof(size_t) * 8 / 2);

	*size = dopts->item_size * dopts->num_items;

	if (unlikely(*size == 0)) {
		return (dopts->num_items != 0 && dopts->item_size != 0);
	}

	/*
	 * We got a non-zero size, but we don't know if we overflowed to get
	 * there.  To avoid having to do a divide, we'll be clever and note that
	 * if both A and B can be represented in N/2 bits, then their product
	 * can be represented in N bits (without the possibility of overflow).
	 */
	if (likely((high_bits & (dopts->num_items | dopts->item_size)) == 0)) {
		return false;
	}
	if (likely(*size / dopts->item_size == dopts->num_items)) {
		return false;
	}
	return true;
}

JEMALLOC_ALWAYS_INLINE int
imalloc_body(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd) {
	/* Where the actual allocated memory will live. */
	void *allocation = NULL;
	/* Filled in by compute_size_with_overflow below. */
	size_t size = 0;
	/*
	 * The zero initialization for ind is actually dead store, in that its
	 * value is reset before any branch on its value is taken.  Sometimes
	 * though, it's convenient to pass it as arguments before this point.
	 * To avoid undefined behavior then, we initialize it with dummy stores.
	 */
	szind_t ind = 0;
	/* usize will always be properly initialized. */
	size_t usize;

	/* Reentrancy is only checked on slow path. */
	int8_t reentrancy_level;

	/* Compute the amount of memory the user wants. */
	if (unlikely(compute_size_with_overflow(
	        sopts->may_overflow, dopts, &size))) {
		goto label_oom;
	}

	if (unlikely(dopts->alignment < sopts->min_alignment
	        || (dopts->alignment & (dopts->alignment - 1)) != 0)) {
		goto label_invalid_alignment;
	}

	/* This is the beginning of the "core" algorithm. */
	dopts->zero = zero_get(dopts->zero, sopts->slow);
	if (aligned_usize_get(size, dopts->alignment, &usize, &ind,
	        sopts->bump_empty_aligned_alloc)) {
		goto label_oom;
	}
	dopts->usize = usize;
	/* Validate the user input. */
	if (sopts->assert_nonempty_alloc) {
		assert(size != 0);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	/*
	 * If we need to handle reentrancy, we can do it out of a
	 * known-initialized arena (i.e. arena 0).
	 */
	reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (sopts->slow && unlikely(reentrancy_level > 0)) {
		/*
		 * We should never specify particular arenas or tcaches from
		 * within our internal allocations.
		 */
		assert(dopts->tcache_ind == TCACHE_IND_AUTOMATIC
		    || dopts->tcache_ind == TCACHE_IND_NONE);
		assert(dopts->arena_ind == ARENA_IND_AUTOMATIC);
		dopts->tcache_ind = TCACHE_IND_NONE;
		/* We know that arena 0 has already been initialized. */
		dopts->arena_ind = 0;
	}

	/*
	 * If dopts->alignment > 0, then ind is still 0, but usize was computed
	 * in the previous if statement.  Down the positive alignment path,
	 * imalloc_no_sample and imalloc_sample will ignore ind.
	 */

	/* If profiling is on, get our profiling context. */
	if (config_prof && opt_prof) {
		bool prof_active = prof_active_get_unlocked();
		bool sample_event = te_prof_sample_event_lookahead(tsd, usize);
		prof_tctx_t *tctx = prof_alloc_prep(
		    tsd, prof_active, sample_event);

		emap_alloc_ctx_t alloc_ctx;
		if (likely(tctx == PROF_TCTX_SENTINEL)) {
			alloc_ctx.slab = sz_can_use_slab(usize);
			allocation = imalloc_no_sample(sopts, dopts, tsd, usize,
			    usize, ind, alloc_ctx.slab);
		} else if (tctx != NULL) {
			allocation = imalloc_sample(
			    sopts, dopts, tsd, usize, ind);
			alloc_ctx.slab = false;
		} else {
			allocation = NULL;
		}

		if (unlikely(allocation == NULL)) {
			prof_alloc_rollback(tsd, tctx);
			goto label_oom;
		}
		prof_malloc(tsd, allocation, size, usize, &alloc_ctx, tctx);
	} else {
		assert(!opt_prof);
		allocation = imalloc_no_sample(sopts, dopts, tsd, size, usize,
		    ind, sz_can_use_slab(usize));
		if (unlikely(allocation == NULL)) {
			goto label_oom;
		}
	}

	/*
	 * Allocation has been done at this point.  We still have some
	 * post-allocation work to do though.
	 */

	thread_alloc_event(tsd, usize);

	assert(dopts->alignment == 0
	    || ((uintptr_t)allocation & (dopts->alignment - 1)) == ZU(0));

	assert(usize == isalloc(tsd_tsdn(tsd), allocation));

	if (config_fill && sopts->slow && !dopts->zero
	    && unlikely(opt_junk_alloc)) {
		junk_alloc_callback(allocation, usize);
	}

	if (sopts->slow) {
		UTRACE(0, size, allocation);
	}

	/* Success! */
	check_entry_exit_locking(tsd_tsdn(tsd));
	*dopts->result = allocation;
	return 0;

label_oom:
	if (unlikely(sopts->slow) && config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->oom_string);
		abort();
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->set_errno_on_error) {
		set_errno(ENOMEM);
	}

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return ENOMEM;

	/*
	 * This label is only jumped to by one goto; we move it out of line
	 * anyways to avoid obscuring the non-error paths, and for symmetry with
	 * the oom case.
	 */
label_invalid_alignment:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->invalid_alignment_string);
		abort();
	}

	if (sopts->set_errno_on_error) {
		set_errno(EINVAL);
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return EINVAL;
}

JEMALLOC_ALWAYS_INLINE bool
imalloc_init_check(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (unlikely(!malloc_initialized()) && unlikely(malloc_init())) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write(sopts->oom_string);
			abort();
		}
		UTRACE(NULL, dopts->num_items * dopts->item_size, NULL);
		set_errno(ENOMEM);
		*dopts->result = NULL;

		return false;
	}

	return true;
}

/* Returns the errno-style error code of the allocation. */
JEMALLOC_ALWAYS_INLINE int
imalloc(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
		return ENOMEM;
	}

	/* We always need the tsd.  Let's grab it right away. */
	tsd_t *tsd = tsd_fetch();
	assert(tsd);
	if (likely(tsd_fast(tsd))) {
		/* Fast and common path. */
		tsd_assert_fast(tsd);
		sopts->slow = false;
		return imalloc_body(sopts, dopts, tsd);
	} else {
		if (!tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
			return ENOMEM;
		}

		sopts->slow = true;
		return imalloc_body(sopts, dopts, tsd);
	}
}

JEMALLOC_NOINLINE
void *
malloc_default(size_t size) {
	void          *ret;
	static_opts_t  sopts;
	dynamic_opts_t dopts;

	/*
	 * This variant has logging hook on exit but not on entry.  It's callled
	 * only by je_malloc, below, which emits the entry one for us (and, if
	 * it calls us, does so only via tail call).
	 */

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in malloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;

	imalloc(&sopts, &dopts);
	/*
	 * Note that this branch gets optimized away -- it immediately follows
	 * the check on tsd_fast that sets sopts.slow.
	 */
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_malloc, ret, (uintptr_t)ret, args);
	}

	return ret;
}

/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1) je_malloc(size_t size) {
	LOG("core.malloc.entry", "size: %zu", size);

	void *ret = imalloc_fastpath(size, &malloc_default);

	LOG("core.malloc.exit", "result: %p", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
JEMALLOC_ATTR(nonnull(1))
    je_posix_memalign(void **memptr, size_t alignment, size_t size) {
	int            ret;
	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.posix_memalign.entry",
	    "mem ptr: %p, alignment: %zu, "
	    "size: %zu",
	    memptr, alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.min_alignment = sizeof(void *);
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = memptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	ret = imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {
		    (uintptr_t)memptr, (uintptr_t)alignment, (uintptr_t)size};
		hook_invoke_alloc(
		    hook_alloc_posix_memalign, *memptr, (uintptr_t)ret, args);
	}

	LOG("core.posix_memalign.exit", "result: %d, alloc ptr: %p", ret,
	    *memptr);

	return ret;
}

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(2)
    je_aligned_alloc(size_t alignment, size_t size) {
	void *ret;

	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.aligned_alloc.entry", "alignment: %zu, size: %zu\n",
	    alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)alignment, (uintptr_t)size};
		hook_invoke_alloc(
		    hook_alloc_aligned_alloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.aligned_alloc.exit", "result: %p", ret);

	return ret;
}

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
    je_calloc(size_t num, size_t size) {
	void          *ret;
	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.calloc.entry", "num: %zu, size: %zu", num, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.may_overflow = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in calloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = num;
	dopts.item_size = size;
	dopts.zero = true;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)num, (uintptr_t)size};
		hook_invoke_alloc(hook_alloc_calloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.calloc.exit", "result: %p", ret);

	return ret;
}

JEMALLOC_ALWAYS_INLINE void
ifree(tsd_t *tsd, void *ptr, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || malloc_is_initializer());

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(
	    tsd_tsdn(tsd), &arena_emap_global, ptr, &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);

	size_t usize = emap_alloc_ctx_usize_get(&alloc_ctx);
	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, &alloc_ctx);
	}

	if (likely(!slow_path)) {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false, false);
	} else {
		if (config_fill && slow_path && opt_junk_free) {
			junk_free_callback(ptr, usize);
		}
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false, true);
	}
	thread_dalloc_event(tsd, usize);
}

JEMALLOC_ALWAYS_INLINE void
isfree(tsd_t *tsd, void *ptr, size_t usize, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || malloc_is_initializer());

	emap_alloc_ctx_t alloc_ctx;
	szind_t          szind = sz_size2index(usize);
	if (!config_prof) {
		emap_alloc_ctx_init(
		    &alloc_ctx, szind, (szind < SC_NBINS), usize);
	} else {
		if (likely(!prof_sample_aligned(ptr))) {
			/*
			 * When the ptr is not page aligned, it was not sampled.
			 * usize can be trusted to determine szind and slab.
			 */
			emap_alloc_ctx_init(
			    &alloc_ctx, szind, (szind < SC_NBINS), usize);
		} else if (opt_prof) {
			/*
			 * Small sampled allocs promoted can still get correct
			 * usize here.  Check comments in edata_usize_get.
			 */
			emap_alloc_ctx_lookup(
			    tsd_tsdn(tsd), &arena_emap_global, ptr, &alloc_ctx);

			if (config_opt_safety_checks) {
				/* Small alloc may have !slab (sampled). */
				size_t true_size = emap_alloc_ctx_usize_get(
				    &alloc_ctx);
				if (unlikely(alloc_ctx.szind
				        != sz_size2index(usize))) {
					safety_check_fail_sized_dealloc(
					    /* current_dealloc */ true, ptr,
					    /* true_size */ true_size,
					    /* input_size */ usize);
				}
			}
		} else {
			emap_alloc_ctx_init(
			    &alloc_ctx, szind, (szind < SC_NBINS), usize);
		}
	}
	bool fail = maybe_check_alloc_ctx(tsd, ptr, &alloc_ctx);
	if (fail) {
		/*
		 * This is a heap corruption bug.  In real life we'll crash; for
		 * the unit test we just want to avoid breaking anything too
		 * badly to get a test result out.  Let's leak instead of trying
		 * to free.
		 */
		return;
	}

	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, &alloc_ctx);
	}
	if (likely(!slow_path)) {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, &alloc_ctx, false);
	} else {
		if (config_fill && slow_path && opt_junk_free) {
			junk_free_callback(ptr, usize);
		}
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, &alloc_ctx, true);
	}
	thread_dalloc_event(tsd, usize);
}

JEMALLOC_NOINLINE
void
free_default(void *ptr) {
	UTRACE(ptr, 0, 0);
	if (likely(ptr != NULL)) {
		/*
		 * We avoid setting up tsd fully (e.g. tcache, arena binding)
		 * based on only free() calls -- other activities trigger the
		 * minimal to full transition.  This is because free() may
		 * happen during thread shutdown after tls deallocation: if a
		 * thread never had any malloc activities until then, a
		 * fully-setup tsd won't be destructed properly.
		 */
		tsd_t *tsd = tsd_fetch_min();
		check_entry_exit_locking(tsd_tsdn(tsd));

		if (likely(tsd_fast(tsd))) {
			tcache_t *tcache = tcache_get_from_ind(tsd,
			    TCACHE_IND_AUTOMATIC, /* slow */ false,
			    /* is_alloc */ false);
			ifree(tsd, ptr, tcache, /* slow */ false);
		} else {
			tcache_t *tcache = tcache_get_from_ind(tsd,
			    TCACHE_IND_AUTOMATIC, /* slow */ true,
			    /* is_alloc */ false);
			uintptr_t args_raw[3] = {(uintptr_t)ptr};
			hook_invoke_dalloc(hook_dalloc_free, ptr, args_raw);
			ifree(tsd, ptr, tcache, /* slow */ true);
		}

		check_entry_exit_locking(tsd_tsdn(tsd));
	}
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free(void *ptr) {
	LOG("core.free.entry", "ptr: %p", ptr);

	je_free_impl(ptr);

	LOG("core.free.exit", "");
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free_sized(void *ptr, size_t size) {
	LOG("core.free_sized.entry", "ptr: %p, size: %zu", ptr, size);

	je_sdallocx_noflags(ptr, size);

	LOG("core.free_sized.exit", "");
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free_aligned_sized(void *ptr, size_t alignment, size_t size) {
	return je_sdallocx(ptr, size, /* flags */ MALLOCX_ALIGN(alignment));
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard override functions.
 */

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) je_memalign(size_t alignment, size_t size) {
	void          *ret;
	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.memalign.entry", "alignment: %zu, size: %zu\n", alignment,
	    size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";
	sopts.null_out_result_on_error = true;

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {alignment, size};
		hook_invoke_alloc(
		    hook_alloc_memalign, ret, (uintptr_t)ret, args);
	}

	LOG("core.memalign.exit", "result: %p", ret);
	return ret;
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) je_valloc(size_t size) {
	void *ret;

	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.valloc.entry", "size: %zu\n", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.min_alignment = PAGE;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = PAGE;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_valloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.valloc.exit", "result: %p\n", ret);
	return ret;
}
#endif

#ifdef JEMALLOC_OVERRIDE_PVALLOC
JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) je_pvalloc(size_t size) {
	void *ret;

	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.pvalloc.entry", "size: %zu\n", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.min_alignment = PAGE;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	/*
	 * This is the only difference from je_valloc - size is rounded up to
	 * a PAGE multiple.
	 */
	dopts.item_size = PAGE_CEILING(size);
	dopts.alignment = PAGE;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(
		    hook_alloc_pvalloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.pvalloc.exit", "result: %p\n", ret);
	return ret;
}
#endif

#if defined(JEMALLOC_IS_MALLOC) && defined(JEMALLOC_GLIBC_MALLOC_HOOK)
/*
 * glibc provides the RTLD_DEEPBIND flag for dlopen which can make it possible
 * to inconsistently reference libc's malloc(3)-compatible functions
 * (https://bugzilla.mozilla.org/show_bug.cgi?id=493541).
 *
 * These definitions interpose hooks in glibc.  The functions are actually
 * passed an extra argument for the caller return address, which will be
 * ignored.
 */
#	include <features.h> // defines __GLIBC__ if we are compiling against glibc

JEMALLOC_EXPORT void (*__free_hook)(void *ptr) = je_free;
JEMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = je_malloc;
JEMALLOC_EXPORT void *(*__realloc_hook)(void *ptr, size_t size) = je_realloc;
#	ifdef JEMALLOC_GLIBC_MEMALIGN_HOOK
JEMALLOC_EXPORT void *(*__memalign_hook)(
    size_t alignment, size_t size) = je_memalign;
#	endif

#	ifdef __GLIBC__
/*
 * To enable static linking with glibc, the libc specific malloc interface must
 * be implemented also, so none of glibc's malloc.o functions are added to the
 * link.
 */
#		define ALIAS(je_fn) __attribute__((alias(#je_fn), used))
/* To force macro expansion of je_ prefix before stringification. */
#		define PREALIAS(je_fn) ALIAS(je_fn)
#		ifdef JEMALLOC_OVERRIDE___LIBC_CALLOC
void *__libc_calloc(size_t n, size_t size) PREALIAS(je_calloc);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_FREE
void __libc_free(void *ptr) PREALIAS(je_free);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_FREE_SIZED
void __libc_free_sized(void *ptr, size_t size) PREALIAS(je_free_sized);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_FREE_ALIGNED_SIZED
void __libc_free_aligned_sized(void *ptr, size_t alignment, size_t size)
    PREALIAS(je_free_aligned_sized);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_MALLOC
void *__libc_malloc(size_t size) PREALIAS(je_malloc);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_MEMALIGN
void *__libc_memalign(size_t align, size_t s) PREALIAS(je_memalign);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_REALLOC
void *__libc_realloc(void *ptr, size_t size) PREALIAS(je_realloc);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_VALLOC
void *__libc_valloc(size_t size) PREALIAS(je_valloc);
#		endif
#		ifdef JEMALLOC_OVERRIDE___LIBC_PVALLOC
void *__libc_pvalloc(size_t size) PREALIAS(je_pvalloc);
#		endif
#		ifdef JEMALLOC_OVERRIDE___POSIX_MEMALIGN
int __posix_memalign(void **r, size_t a, size_t s) PREALIAS(je_posix_memalign);
#		endif
#		undef PREALIAS
#		undef ALIAS
#	endif
#endif

/*
 * End non-standard override functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

JEMALLOC_ALWAYS_INLINE unsigned
mallocx_tcache_get(int flags) {
	if (likely((flags & MALLOCX_TCACHE_MASK) == 0)) {
		return TCACHE_IND_AUTOMATIC;
	} else if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
		return TCACHE_IND_NONE;
	} else {
		return MALLOCX_TCACHE_GET(flags);
	}
}

JEMALLOC_ALWAYS_INLINE unsigned
mallocx_arena_get(int flags) {
	if (unlikely((flags & MALLOCX_ARENA_MASK) != 0)) {
		return MALLOCX_ARENA_GET(flags);
	} else {
		return ARENA_IND_AUTOMATIC;
	}
}

#ifdef JEMALLOC_EXPERIMENTAL_SMALLOCX_API

#	define JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y) x##y
#	define JEMALLOC_SMALLOCX_CONCAT_HELPER2(x, y)                         \
		JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y)

typedef struct {
	void  *ptr;
	size_t size;
} smallocx_return_t;

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN smallocx_return_t
    JEMALLOC_NOTHROW
    /*
 * The attribute JEMALLOC_ATTR(malloc) cannot be used due to:
 *  - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=86488
 */
    JEMALLOC_SMALLOCX_CONCAT_HELPER2(je_smallocx_, JEMALLOC_VERSION_GID_IDENT)(
        size_t size, int flags) {
	/*
	 * Note: the attribute JEMALLOC_ALLOC_SIZE(1) cannot be
	 * used here because it makes writing beyond the `size`
	 * of the `ptr` undefined behavior, but the objective
	 * of this function is to allow writing beyond `size`
	 * up to `smallocx_return_t::size`.
	 */
	smallocx_return_t ret;
	static_opts_t     sopts;
	dynamic_opts_t    dopts;

	LOG("core.smallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";
	sopts.usize = true;

	dopts.result = &ret.ptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		dopts.alignment = MALLOCX_ALIGN_GET(flags);
		dopts.zero = MALLOCX_ZERO_GET(flags);
		dopts.tcache_ind = mallocx_tcache_get(flags);
		dopts.arena_ind = mallocx_arena_get(flags);
	}

	imalloc(&sopts, &dopts);
	assert(dopts.usize == je_nallocx(size, flags));
	ret.size = dopts.usize;

	LOG("core.smallocx.exit", "result: %p, size: %zu", ret.ptr, ret.size);
	return ret;
}
#	undef JEMALLOC_SMALLOCX_CONCAT_HELPER
#	undef JEMALLOC_SMALLOCX_CONCAT_HELPER2
#endif

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
    je_mallocx(size_t size, int flags) {
	void          *ret;
	static_opts_t  sopts;
	dynamic_opts_t dopts;

	LOG("core.mallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		dopts.alignment = MALLOCX_ALIGN_GET(flags);
		dopts.zero = MALLOCX_ZERO_GET(flags);
		dopts.tcache_ind = mallocx_tcache_get(flags);
		dopts.arena_ind = mallocx_arena_get(flags);
	}

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size, flags};
		hook_invoke_alloc(
		    hook_alloc_mallocx, ret, (uintptr_t)ret, args);
	}

	LOG("core.mallocx.exit", "result: %p", ret);
	return ret;
}

static void *
irallocx_prof_sample(tsdn_t *tsdn, void *old_ptr, size_t old_usize,
    size_t usize, size_t alignment, bool zero, tcache_t *tcache, arena_t *arena,
    prof_tctx_t *tctx, hook_ralloc_args_t *hook_args) {
	void *p;

	if (tctx == NULL) {
		return NULL;
	}

	alignment = prof_sample_align(usize, alignment);
	/*
	 * If the allocation is small enough that it would normally be allocated
	 * on a slab, we need to take additional steps to ensure that it gets
	 * its own extent instead.
	 */
	if (sz_can_use_slab(usize)) {
		size_t bumped_usize = sz_sa2u(usize, alignment);
		p = iralloct_explicit_slab(tsdn, old_ptr, old_usize,
		    bumped_usize, alignment, zero, /* slab */ false, tcache,
		    arena, hook_args);
		if (p == NULL) {
			return NULL;
		}
		arena_prof_promote(tsdn, p, usize, bumped_usize);
	} else {
		p = iralloct_explicit_slab(tsdn, old_ptr, old_usize, usize,
		    alignment, zero, /* slab */ false, tcache, arena,
		    hook_args);
	}
	assert(prof_sample_aligned(p));

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irallocx_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t size,
    size_t alignment, size_t usize, bool zero, tcache_t *tcache, arena_t *arena,
    emap_alloc_ctx_t *alloc_ctx, hook_ralloc_args_t *hook_args) {
	prof_info_t old_prof_info;
	prof_info_get_and_reset_recent(tsd, old_ptr, alloc_ctx, &old_prof_info);
	bool         prof_active = prof_active_get_unlocked();
	bool         sample_event = te_prof_sample_event_lookahead(tsd, usize);
	prof_tctx_t *tctx = prof_alloc_prep(tsd, prof_active, sample_event);
	void        *p;
	if (unlikely(tctx != PROF_TCTX_SENTINEL)) {
		p = irallocx_prof_sample(tsd_tsdn(tsd), old_ptr, old_usize,
		    usize, alignment, zero, tcache, arena, tctx, hook_args);
	} else {
		p = iralloct(tsd_tsdn(tsd), old_ptr, old_usize, size, alignment,
		    usize, zero, tcache, arena, hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx);
		return NULL;
	}
	assert(usize == isalloc(tsd_tsdn(tsd), p));
	prof_realloc(tsd, p, size, usize, tctx, prof_active, old_ptr, old_usize,
	    &old_prof_info, sample_event);

	return p;
}

static void *
do_rallocx(void *ptr, size_t size, int flags, bool is_realloc) {
	void    *p;
	tsd_t   *tsd;
	size_t   usize;
	size_t   old_usize;
	size_t   alignment = MALLOCX_ALIGN_GET(flags);
	arena_t *arena;

	assert(ptr != NULL);
	assert(size != 0);
	assert(malloc_initialized() || malloc_is_initializer());
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	bool zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	unsigned arena_ind = mallocx_arena_get(flags);
	if (arena_get_from_ind(tsd, arena_ind, &arena)) {
		goto label_oom;
	}

	unsigned  tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind,
	    /* slow */ true, /* is_alloc */ true);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(
	    tsd_tsdn(tsd), &arena_emap_global, ptr, &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = emap_alloc_ctx_usize_get(&alloc_ctx);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
	if (aligned_usize_get(size, alignment, &usize, NULL, false)) {
		goto label_oom;
	}

	hook_ralloc_args_t hook_args = {
	    is_realloc, {(uintptr_t)ptr, size, flags, 0}};
	if (config_prof && opt_prof) {
		p = irallocx_prof(tsd, ptr, old_usize, size, alignment, usize,
		    zero, tcache, arena, &alloc_ctx, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
	} else {
		p = iralloct(tsd_tsdn(tsd), ptr, old_usize, size, alignment,
		    usize, zero, tcache, arena, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
		assert(usize == isalloc(tsd_tsdn(tsd), p));
	}
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));
	thread_alloc_event(tsd, usize);
	thread_dalloc_event(tsd, old_usize);

	UTRACE(ptr, size, p);
	check_entry_exit_locking(tsd_tsdn(tsd));

	if (config_fill && unlikely(opt_junk_alloc) && usize > old_usize
	    && !zero) {
		size_t excess_len = usize - old_usize;
		void  *excess_start = (void *)((byte_t *)p + old_usize);
		junk_alloc_callback(excess_start, excess_len);
	}

	return p;
label_oom:
	if (is_realloc) {
		set_errno(ENOMEM);
	}
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in rallocx(): out of memory\n");
		abort();
	}
	UTRACE(ptr, size, 0);
	check_entry_exit_locking(tsd_tsdn(tsd));

	return NULL;
}

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2) je_rallocx(void *ptr, size_t size, int flags) {
	LOG("core.rallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr, size,
	    flags);
	void *ret = do_rallocx(ptr, size, flags, false);
	LOG("core.rallocx.exit", "result: %p", ret);
	return ret;
}

static void *
do_realloc_nonnull_zero(void *ptr) {
	if (config_stats) {
		atomic_fetch_add_zu(&zero_realloc_count, 1, ATOMIC_RELAXED);
	}
	if (opt_zero_realloc_action == zero_realloc_action_alloc) {
		/*
		 * The user might have gotten an alloc setting while expecting a
		 * free setting.  If that's the case, we at least try to
		 * reduce the harm, and turn off the tcache while allocating, so
		 * that we'll get a true first fit.
		 */
		return do_rallocx(ptr, 1, MALLOCX_TCACHE_NONE, true);
	} else if (opt_zero_realloc_action == zero_realloc_action_free) {
		UTRACE(ptr, 0, 0);
		tsd_t *tsd = tsd_fetch();
		check_entry_exit_locking(tsd_tsdn(tsd));

		tcache_t *tcache = tcache_get_from_ind(tsd,
		    TCACHE_IND_AUTOMATIC, /* slow */ true,
		    /* is_alloc */ false);
		uintptr_t args[3] = {(uintptr_t)ptr, 0};
		hook_invoke_dalloc(hook_dalloc_realloc, ptr, args);
		ifree(tsd, ptr, tcache, true);

		check_entry_exit_locking(tsd_tsdn(tsd));
		return NULL;
	} else {
		safety_check_fail(
		    "Called realloc(non-null-ptr, 0) with "
		    "zero_realloc:abort set\n");
		/* In real code, this will never run; the safety check failure
		 * will call abort.  In the unit test, we just want to bail out
		 * without corrupting internal state that the test needs to
		 * finish.
		 */
		return NULL;
	}
}

JEMALLOC_EXPORT
JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2) je_realloc(void *ptr, size_t size) {
	LOG("core.realloc.entry", "ptr: %p, size: %zu\n", ptr, size);

	if (likely(ptr != NULL && size != 0)) {
		void *ret = do_rallocx(ptr, size, 0, true);
		LOG("core.realloc.exit", "result: %p", ret);
		return ret;
	} else if (ptr != NULL && size == 0) {
		void *ret = do_realloc_nonnull_zero(ptr);
		LOG("core.realloc.exit", "result: %p", ret);
		return ret;
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		void *ret;

		static_opts_t  sopts;
		dynamic_opts_t dopts;

		static_opts_init(&sopts);
		dynamic_opts_init(&dopts);

		sopts.null_out_result_on_error = true;
		sopts.set_errno_on_error = true;
		sopts.oom_string =
		    "<jemalloc>: Error in realloc(): out of memory\n";

		dopts.result = &ret;
		dopts.num_items = 1;
		dopts.item_size = size;

		imalloc(&sopts, &dopts);
		if (sopts.slow) {
			uintptr_t args[3] = {(uintptr_t)ptr, size};
			hook_invoke_alloc(
			    hook_alloc_realloc, ret, (uintptr_t)ret, args);
		}
		LOG("core.realloc.exit", "result: %p", ret);
		return ret;
	}
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_helper(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero) {
	size_t newsize;

	if (ixalloc(
	        tsdn, ptr, old_usize, size, extra, alignment, zero, &newsize)) {
		return old_usize;
	}

	return newsize;
}

static size_t
ixallocx_prof_sample(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, prof_tctx_t *tctx) {
	/* Sampled allocation needs to be page aligned. */
	if (tctx == NULL || !prof_sample_aligned(ptr)) {
		return old_usize;
	}

	return ixallocx_helper(
	    tsdn, ptr, old_usize, size, extra, alignment, zero);
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_prof(tsd_t *tsd, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, emap_alloc_ctx_t *alloc_ctx) {
	/*
	 * old_prof_info is only used for asserting that the profiling info
	 * isn't changed by the ixalloc() call.
	 */
	prof_info_t old_prof_info;
	prof_info_get(tsd, ptr, alloc_ctx, &old_prof_info);

	/*
	 * usize isn't knowable before ixalloc() returns when extra is non-zero.
	 * Therefore, compute its maximum possible value and use that in
	 * prof_alloc_prep() to decide whether to capture a backtrace.
	 * prof_realloc() will use the actual usize to decide whether to sample.
	 */
	size_t usize_max;
	if (aligned_usize_get(
	        size + extra, alignment, &usize_max, NULL, false)) {
		/*
		 * usize_max is out of range, and chances are that allocation
		 * will fail, but use the maximum possible value and carry on
		 * with prof_alloc_prep(), just in case allocation succeeds.
		 */
		usize_max = SC_LARGE_MAXCLASS;
	}
	bool prof_active = prof_active_get_unlocked();
	bool sample_event = te_prof_sample_event_lookahead(tsd, usize_max);
	prof_tctx_t *tctx = prof_alloc_prep(tsd, prof_active, sample_event);

	size_t usize;
	if (unlikely(tctx != PROF_TCTX_SENTINEL)) {
		usize = ixallocx_prof_sample(tsd_tsdn(tsd), ptr, old_usize,
		    size, extra, alignment, zero, tctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}

	/*
	 * At this point we can still safely get the original profiling
	 * information associated with the ptr, because (a) the edata_t object
	 * associated with the ptr still lives and (b) the profiling info
	 * fields are not touched.  "(a)" is asserted in the outer je_xallocx()
	 * function, and "(b)" is indirectly verified below by checking that
	 * the alloc_tctx field is unchanged.
	 */
	prof_info_t prof_info;
	if (usize == old_usize) {
		prof_info_get(tsd, ptr, alloc_ctx, &prof_info);
		prof_alloc_rollback(tsd, tctx);
	} else {
		/*
		 * Need to retrieve the new alloc_ctx since the modification
		 * to edata has already been done.
		 */
		emap_alloc_ctx_t new_alloc_ctx;
		emap_alloc_ctx_lookup(
		    tsd_tsdn(tsd), &arena_emap_global, ptr, &new_alloc_ctx);
		prof_info_get_and_reset_recent(
		    tsd, ptr, &new_alloc_ctx, &prof_info);
		assert(usize <= usize_max);
		sample_event = te_prof_sample_event_lookahead(tsd, usize);
		prof_realloc(tsd, ptr, size, usize, tctx, prof_active, ptr,
		    old_usize, &prof_info, sample_event);
	}

	assert(old_prof_info.alloc_tctx == prof_info.alloc_tctx);
	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_xallocx(void *ptr, size_t size, size_t extra, int flags) {
	tsd_t *tsd;
	size_t usize, old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool   zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	LOG("core.xallocx.entry",
	    "ptr: %p, size: %zu, extra: %zu, "
	    "flags: %d",
	    ptr, size, extra, flags);

	assert(ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized() || malloc_is_initializer());
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	/*
	 * old_edata is only for verifying that xallocx() keeps the edata_t
	 * object associated with the ptr (though the content of the edata_t
	 * object can be changed).
	 */
	edata_t *old_edata = emap_edata_lookup(
	    tsd_tsdn(tsd), &arena_emap_global, ptr);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(
	    tsd_tsdn(tsd), &arena_emap_global, ptr, &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = emap_alloc_ctx_usize_get(&alloc_ctx);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
	/*
	 * The API explicitly absolves itself of protecting against (size +
	 * extra) numerical overflow, but we may need to clamp extra to avoid
	 * exceeding SC_LARGE_MAXCLASS.
	 *
	 * Ordinarily, size limit checking is handled deeper down, but here we
	 * have to check as part of (size + extra) clamping, since we need the
	 * clamped value in the above helper functions.
	 */
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		usize = old_usize;
		goto label_not_resized;
	}
	if (unlikely(SC_LARGE_MAXCLASS - size < extra)) {
		extra = SC_LARGE_MAXCLASS - size;
	}

	if (config_prof && opt_prof) {
		usize = ixallocx_prof(tsd, ptr, old_usize, size, extra,
		    alignment, zero, &alloc_ctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}

	/*
	 * xallocx() should keep using the same edata_t object (though its
	 * content can be changed).
	 */
	assert(emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr)
	    == old_edata);

	if (unlikely(usize == old_usize)) {
		goto label_not_resized;
	}
	thread_alloc_event(tsd, usize);
	thread_dalloc_event(tsd, old_usize);

	if (config_fill && unlikely(opt_junk_alloc) && usize > old_usize
	    && !zero) {
		size_t excess_len = usize - old_usize;
		void  *excess_start = (void *)((byte_t *)ptr + old_usize);
		junk_alloc_callback(excess_start, excess_len);
	}
label_not_resized:
	if (unlikely(!tsd_fast(tsd))) {
		uintptr_t args[4] = {(uintptr_t)ptr, size, extra, flags};
		hook_invoke_expand(hook_expand_xallocx, ptr, old_usize, usize,
		    (uintptr_t)usize, args);
	}

	UTRACE(ptr, size, ptr);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.xallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure) je_sallocx(const void *ptr, int flags) {
	size_t  usize;
	tsdn_t *tsdn;

	LOG("core.sallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(malloc_initialized() || malloc_is_initializer());
	assert(ptr != NULL);

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (config_debug || force_ivsalloc) {
		usize = ivsalloc(tsdn, ptr);
		assert(force_ivsalloc || usize != 0);
	} else {
		usize = isalloc(tsdn, ptr);
	}

	check_entry_exit_locking(tsdn);

	LOG("core.sallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_dallocx(void *ptr, int flags) {
	LOG("core.dallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(ptr != NULL);
	assert(malloc_initialized() || malloc_is_initializer());

	tsd_t *tsd = tsd_fetch_min();
	bool   fast = tsd_fast(tsd);
	check_entry_exit_locking(tsd_tsdn(tsd));

	unsigned  tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind, !fast,
	    /* is_alloc */ false);

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		ifree(tsd, ptr, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, flags};
		hook_invoke_dalloc(hook_dalloc_dallocx, ptr, args_raw);
		ifree(tsd, ptr, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.dallocx.exit", "");
}

JEMALLOC_ALWAYS_INLINE size_t
inallocx(tsdn_t *tsdn, size_t size, int flags) {
	check_entry_exit_locking(tsdn);
	size_t usize;
	/* In case of out of range, let the user see it rather than fail. */
	aligned_usize_get(size, MALLOCX_ALIGN_GET(flags), &usize, NULL, false);
	check_entry_exit_locking(tsdn);
	return usize;
}

JEMALLOC_NOINLINE void
sdallocx_default(void *ptr, size_t size, int flags) {
	assert(ptr != NULL);
	assert(malloc_initialized() || malloc_is_initializer());

	tsd_t *tsd = tsd_fetch_min();
	bool   fast = tsd_fast(tsd);
	size_t usize = inallocx(tsd_tsdn(tsd), size, flags);
	check_entry_exit_locking(tsd_tsdn(tsd));

	unsigned  tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind, !fast,
	    /* is_alloc */ false);

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		isfree(tsd, ptr, usize, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, size, flags};
		hook_invoke_dalloc(hook_dalloc_sdallocx, ptr, args_raw);
		isfree(tsd, ptr, usize, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_sdallocx(void *ptr, size_t size, int flags) {
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr, size,
	    flags);

	je_sdallocx_impl(ptr, size, flags);

	LOG("core.sdallocx.exit", "");
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure) je_nallocx(size_t size, int flags) {
	size_t  usize;
	tsdn_t *tsdn;

	assert(size != 0);

	if (unlikely(malloc_init())) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	usize = inallocx(tsdn, size, flags);
	if (unlikely(usize > SC_LARGE_MAXCLASS)) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	check_entry_exit_locking(tsdn);
	LOG("core.nallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctl(
    const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int    ret;
	tsd_t *tsd;

	LOG("core.mallctl.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctl.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_byname(tsd, name, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctl.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp) {
	int ret;

	LOG("core.mallctlnametomib.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctlnametomib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd_t *tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_nametomib(tsd, name, mibp, miblenp);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctlnametomib.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen) {
	int    ret;
	tsd_t *tsd;

	LOG("core.mallctlbymib.entry", "");

	if (unlikely(malloc_init())) {
		LOG("core.mallctlbymib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_bymib(tsd, mib, miblen, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));
	LOG("core.mallctlbymib.exit", "result: %d", ret);
	return ret;
}

#define STATS_PRINT_BUFSIZE 65536
JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_malloc_stats_print(
    void (*write_cb)(void *, const char *), void *cbopaque, const char *opts) {
	tsdn_t *tsdn;

	LOG("core.malloc_stats_print.entry", "");

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (config_debug) {
		stats_print(write_cb, cbopaque, opts);
	} else {
		buf_writer_t buf_writer;
		buf_writer_init(tsdn, &buf_writer, write_cb, cbopaque, NULL,
		    STATS_PRINT_BUFSIZE);
		stats_print(buf_writer_cb, &buf_writer, opts);
		buf_writer_terminate(tsdn, &buf_writer);
	}

	check_entry_exit_locking(tsdn);
	LOG("core.malloc_stats_print.exit", "");
}
#undef STATS_PRINT_BUFSIZE

JEMALLOC_ALWAYS_INLINE size_t
je_malloc_usable_size_impl(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	assert(malloc_initialized() || malloc_is_initializer());

	tsdn_t *tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	size_t ret;
	if (unlikely(ptr == NULL)) {
		ret = 0;
	} else {
		if (config_debug || force_ivsalloc) {
			ret = ivsalloc(tsdn, ptr);
			assert(force_ivsalloc || ret != 0);
		} else {
			ret = isalloc(tsdn, ptr);
		}
	}
	check_entry_exit_locking(tsdn);

	return ret;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_usable_size(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	LOG("core.malloc_usable_size.entry", "ptr: %p", ptr);

	size_t ret = je_malloc_usable_size_impl(ptr);

	LOG("core.malloc_usable_size.exit", "result: %zu", ret);
	return ret;
}

#ifdef JEMALLOC_HAVE_MALLOC_SIZE
JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_size(const void *ptr) {
	LOG("core.malloc_size.entry", "ptr: %p", ptr);

	size_t ret = je_malloc_usable_size_impl(ptr);

	LOG("core.malloc_size.exit", "result: %zu", ret);
	return ret;
}
#endif

static void
batch_alloc_prof_sample_assert(tsd_t *tsd, size_t batch, size_t usize) {
	assert(config_prof && opt_prof);
	bool prof_sample_event = te_prof_sample_event_lookahead(
	    tsd, batch * usize);
	assert(!prof_sample_event);
	size_t surplus;
	prof_sample_event = te_prof_sample_event_lookahead_surplus(
	    tsd, (batch + 1) * usize, &surplus);
	assert(prof_sample_event);
	assert(surplus < usize);
}

size_t
batch_alloc(void **ptrs, size_t num, size_t size, int flags) {
	LOG("core.batch_alloc.entry",
	    "ptrs: %p, num: %zu, size: %zu, flags: %d", ptrs, num, size, flags);

	tsd_t *tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	size_t filled = 0;

	if (unlikely(tsd == NULL || tsd_reentrancy_level_get(tsd) > 0)) {
		goto label_done;
	}

	size_t alignment = MALLOCX_ALIGN_GET(flags);
	size_t usize;
	if (aligned_usize_get(size, alignment, &usize, NULL, false)) {
		goto label_done;
	}
	szind_t ind = sz_size2index(usize);
	bool    zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	/*
	 * The cache bin and arena will be lazily initialized; it's hard to
	 * know in advance whether each of them needs to be initialized.
	 */
	cache_bin_t *bin = NULL;
	arena_t     *arena = NULL;

	size_t nregs = 0;
	if (likely(ind < SC_NBINS)) {
		nregs = bin_infos[ind].nregs;
		assert(nregs > 0);
	}

	while (filled < num) {
		size_t batch = num - filled;
		size_t surplus = SIZE_MAX; /* Dead store. */
		bool   prof_sample_event = config_prof && opt_prof
		    && prof_active_get_unlocked()
		    && te_prof_sample_event_lookahead_surplus(
		        tsd, batch * usize, &surplus);

		if (prof_sample_event) {
			/*
			 * Adjust so that the batch does not trigger prof
			 * sampling.
			 */
			batch -= surplus / usize + 1;
			batch_alloc_prof_sample_assert(tsd, batch, usize);
		}

		size_t progress = 0;

		if (likely(ind < SC_NBINS) && batch >= nregs) {
			if (arena == NULL) {
				unsigned arena_ind = mallocx_arena_get(flags);
				if (arena_get_from_ind(
				        tsd, arena_ind, &arena)) {
					goto label_done;
				}
				if (arena == NULL) {
					arena = arena_choose(tsd, NULL);
				}
				if (unlikely(arena == NULL)) {
					goto label_done;
				}
			}
			size_t arena_batch = batch - batch % nregs;
			size_t n = arena_fill_small_fresh(tsd_tsdn(tsd), arena,
			    ind, ptrs + filled, arena_batch, zero);
			progress += n;
			filled += n;
		}

		unsigned  tcache_ind = mallocx_tcache_get(flags);
		tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind,
		    /* slow */ true, /* is_alloc */ true);
		if (likely(tcache != NULL
		        && ind < tcache_nbins_get(tcache->tcache_slow)
		        && !tcache_bin_disabled(
		            ind, &tcache->bins[ind], tcache->tcache_slow))
		    && progress < batch) {
			if (bin == NULL) {
				bin = &tcache->bins[ind];
			}
			/*
			 * If we don't have a tcache bin, we don't want to
			 * immediately give up, because there's the possibility
			 * that the user explicitly requested to bypass the
			 * tcache, or that the user explicitly turned off the
			 * tcache; in such cases, we go through the slow path,
			 * i.e. the mallocx() call at the end of the while loop.
			 */
			if (bin != NULL) {
				size_t bin_batch = batch - progress;
				/*
				 * n can be less than bin_batch, meaning that
				 * the cache bin does not have enough memory.
				 * In such cases, we rely on the slow path,
				 * i.e. the mallocx() call at the end of the
				 * while loop, to fill in the cache, and in the
				 * next iteration of the while loop, the tcache
				 * will contain a lot of memory, and we can
				 * harvest them here.  Compared to the
				 * alternative approach where we directly go to
				 * the arena bins here, the overhead of our
				 * current approach should usually be minimal,
				 * since we never try to fetch more memory than
				 * what a slab contains via the tcache.  An
				 * additional benefit is that the tcache will
				 * not be empty for the next allocation request.
				 */
				size_t n = cache_bin_alloc_batch(
				    bin, bin_batch, ptrs + filled);
				if (config_stats) {
					bin->tstats.nrequests += n;
				}
				if (zero) {
					for (size_t i = 0; i < n; ++i) {
						memset(
						    ptrs[filled + i], 0, usize);
					}
				}
				if (config_prof && opt_prof
				    && unlikely(ind >= SC_NBINS)) {
					for (size_t i = 0; i < n; ++i) {
						prof_tctx_reset_sampled(
						    tsd, ptrs[filled + i]);
					}
				}
				progress += n;
				filled += n;
			}
		}

		/*
		 * For thread events other than prof sampling, trigger them as
		 * if there's a single allocation of size (n * usize).  This is
		 * fine because:
		 * (a) these events do not alter the allocation itself, and
		 * (b) it's possible that some event would have been triggered
		 *     multiple times, instead of only once, if the allocations
		 *     were handled individually, but it would do no harm (or
		 *     even be beneficial) to coalesce the triggerings.
		 */
		thread_alloc_event(tsd, progress * usize);

		if (progress < batch || prof_sample_event) {
			void *p = je_mallocx(size, flags);
			if (p == NULL) { /* OOM */
				break;
			}
			if (progress == batch) {
				assert(prof_sampled(tsd, p));
			}
			ptrs[filled++] = p;
		}
	}

label_done:
	check_entry_exit_locking(tsd_tsdn(tsd));
	LOG("core.batch_alloc.exit", "result: %zu", filled);
	return filled;
}

/*
 * End non-standard functions.
 */
/******************************************************************************/
