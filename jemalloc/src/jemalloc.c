/*-
 * This allocator implementation is designed to provide scalable performance
 * for multi-threaded programs on multi-processor systems.  The following
 * features are included for this purpose:
 *
 *   + Multiple arenas are used if there are multiple CPUs, which reduces lock
 *     contention and cache sloshing.
 *
 *   + Thread-specific caching is used if there are multiple threads, which
 *     reduces the amount of locking.
 *
 *   + Cache line sharing between arenas is avoided for internal data
 *     structures.
 *
 *   + Memory is managed in chunks and runs (chunks can be split into runs),
 *     rather than as individual pages.  This provides a constant-time
 *     mechanism for associating allocations with particular arenas.
 *
 * Allocation requests are rounded up to the nearest size class, and no record
 * of the original request size is maintained.  Allocations are broken into
 * categories according to size class.  Assuming 1 MiB chunks, 4 KiB pages and
 * a 16 byte quantum on a 32-bit system, the size classes in each category are
 * as follows:
 *
 *   |========================================|
 *   | Category | Subcategory      |     Size |
 *   |========================================|
 *   | Small    | Tiny             |        2 |
 *   |          |                  |        4 |
 *   |          |                  |        8 |
 *   |          |------------------+----------|
 *   |          | Quantum-spaced   |       16 |
 *   |          |                  |       32 |
 *   |          |                  |       48 |
 *   |          |                  |      ... |
 *   |          |                  |       96 |
 *   |          |                  |      112 |
 *   |          |                  |      128 |
 *   |          |------------------+----------|
 *   |          | Cacheline-spaced |      192 |
 *   |          |                  |      256 |
 *   |          |                  |      320 |
 *   |          |                  |      384 |
 *   |          |                  |      448 |
 *   |          |                  |      512 |
 *   |          |------------------+----------|
 *   |          | Sub-page         |      760 |
 *   |          |                  |     1024 |
 *   |          |                  |     1280 |
 *   |          |                  |      ... |
 *   |          |                  |     3328 |
 *   |          |                  |     3584 |
 *   |          |                  |     3840 |
 *   |========================================|
 *   | Large                       |    4 KiB |
 *   |                             |    8 KiB |
 *   |                             |   12 KiB |
 *   |                             |      ... |
 *   |                             | 1012 KiB |
 *   |                             | 1016 KiB |
 *   |                             | 1020 KiB |
 *   |========================================|
 *   | Huge                        |    1 MiB |
 *   |                             |    2 MiB |
 *   |                             |    3 MiB |
 *   |                             |      ... |
 *   |========================================|
 *
 * Different mechanisms are used accoding to category:
 *
 *   Small: Each size class is segregated into its own set of runs.  Each run
 *          maintains a bitmap of which regions are free/allocated.
 *
 *   Large : Each allocation is backed by a dedicated run.  Metadata are stored
 *           in the associated arena chunk header maps.
 *
 *   Huge : Each allocation is backed by a dedicated contiguous set of chunks.
 *          Metadata are stored in a separate red-black tree.
 *
 *******************************************************************************
 */

#define	JEMALLOC_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

malloc_mutex_t		arenas_lock;
arena_t			**arenas;
unsigned		narenas;
#ifndef NO_TLS
static unsigned		next_arena;
#endif

#ifndef NO_TLS
__thread arena_t	*arenas_map JEMALLOC_ATTR(tls_model("initial-exec"));
#endif

/* Set to true once the allocator has been initialized. */
static bool malloc_initialized = false;

/* Used to let the initializing thread recursively allocate. */
static pthread_t malloc_initializer = (unsigned long)0;

/* Used to avoid initialization races. */
static malloc_mutex_t init_lock = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

#ifdef DYNAMIC_PAGE_SHIFT
size_t		pagesize;
size_t		pagesize_mask;
size_t		lg_pagesize;
#endif

unsigned	ncpus;

/* Runtime configuration options. */
const char	*JEMALLOC_P(malloc_options)
    JEMALLOC_ATTR(visibility("default"));
#ifdef JEMALLOC_DEBUG
bool	opt_abort = true;
#  ifdef JEMALLOC_FILL
bool	opt_junk = true;
#  endif
#else
bool	opt_abort = false;
#  ifdef JEMALLOC_FILL
bool	opt_junk = false;
#  endif
#endif
#ifdef JEMALLOC_SYSV
bool	opt_sysv = false;
#endif
#ifdef JEMALLOC_XMALLOC
bool	opt_xmalloc = false;
#endif
#ifdef JEMALLOC_FILL
bool	opt_zero = false;
#endif
static int	opt_narenas_lshift = 0;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	wrtmessage(void *cbopaque, const char *s);
static void	stats_print_atexit(void);
static unsigned	malloc_ncpus(void);
static bool	malloc_init_hard(void);
static void	jemalloc_prefork(void);
static void	jemalloc_postfork(void);

/******************************************************************************/
/* malloc_message() setup. */

#ifdef JEMALLOC_HAVE_ATTR
JEMALLOC_ATTR(visibility("hidden"))
#else
static
#endif
void
wrtmessage(void *cbopaque, const char *s)
{

	write(STDERR_FILENO, s, strlen(s));
}

void	(*JEMALLOC_P(malloc_message))(void *, const char *s)
    JEMALLOC_ATTR(visibility("default")) = wrtmessage;

/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

/* Create a new arena and insert it into the arenas array at index ind. */
arena_t *
arenas_extend(unsigned ind)
{
	arena_t *ret;

	/* Allocate enough space for trailing bins. */
	ret = (arena_t *)base_alloc(sizeof(arena_t)
	    + (sizeof(arena_bin_t) * (nbins - 1)));
	if (ret != NULL && arena_new(ret, ind) == false) {
		arenas[ind] = ret;
		return (ret);
	}
	/* Only reached if there is an OOM error. */

	/*
	 * OOM here is quite inconvenient to propagate, since dealing with it
	 * would require a check for failure in the fast path.  Instead, punt
	 * by using arenas[0].  In practice, this is an extremely unlikely
	 * failure.
	 */
	malloc_write("<jemalloc>: Error initializing arena\n");
	if (opt_abort)
		abort();

	return (arenas[0]);
}

#ifndef NO_TLS
/*
 * Choose an arena based on a per-thread value (slow-path code only, called
 * only by choose_arena()).
 */
arena_t *
choose_arena_hard(void)
{
	arena_t *ret;

	if (narenas > 1) {
		malloc_mutex_lock(&arenas_lock);
		if ((ret = arenas[next_arena]) == NULL)
			ret = arenas_extend(next_arena);
		next_arena = (next_arena + 1) % narenas;
		malloc_mutex_unlock(&arenas_lock);
	} else
		ret = arenas[0];

	arenas_map = ret;

	return (ret);
}
#endif

static void
stats_print_atexit(void)
{

#if (defined(JEMALLOC_TCACHE) && defined(JEMALLOC_STATS))
	unsigned i;

	/*
	 * Merge stats from extant threads.  This is racy, since individual
	 * threads do not lock when recording tcache stats events.  As a
	 * consequence, the final stats may be slightly out of date by the time
	 * they are reported, if other threads continue to allocate.
	 */
	for (i = 0; i < narenas; i++) {
		arena_t *arena = arenas[i];
		if (arena != NULL) {
			tcache_t *tcache;

			/*
			 * tcache_stats_merge() locks bins, so if any code is
			 * introduced that acquires both arena and bin locks in
			 * the opposite order, deadlocks may result.
			 */
			malloc_mutex_lock(&arena->lock);
			ql_foreach(tcache, &arena->tcache_ql, link) {
				tcache_stats_merge(tcache, arena);
			}
			malloc_mutex_unlock(&arena->lock);
		}
	}
#endif
	JEMALLOC_P(malloc_stats_print)(NULL, NULL, NULL);
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin initialization functions.
 */

static unsigned
malloc_ncpus(void)
{
	unsigned ret;
	long result;

	result = sysconf(_SC_NPROCESSORS_ONLN);
	if (result == -1) {
		/* Error. */
		ret = 1;
	}
	ret = (unsigned)result;

	return (ret);
}

/*
 * FreeBSD's pthreads implementation calls malloc(3), so the malloc
 * implementation has to take pains to avoid infinite recursion during
 * initialization.
 */
static inline bool
malloc_init(void)
{

	if (malloc_initialized == false)
		return (malloc_init_hard());

	return (false);
}

static bool
malloc_init_hard(void)
{
	unsigned i;
	int linklen;
	char buf[PATH_MAX + 1];
	const char *opts;
	arena_t *init_arenas[1];

	malloc_mutex_lock(&init_lock);
	if (malloc_initialized || malloc_initializer == pthread_self()) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		malloc_mutex_unlock(&init_lock);
		return (false);
	}
	if (malloc_initializer != (unsigned long)0) {
		/* Busy-wait until the initializing thread completes. */
		do {
			malloc_mutex_unlock(&init_lock);
			CPU_SPINWAIT;
			malloc_mutex_lock(&init_lock);
		} while (malloc_initialized == false);
		return (false);
	}

#ifdef DYNAMIC_PAGE_SHIFT
	/* Get page size. */
	{
		long result;

		result = sysconf(_SC_PAGESIZE);
		assert(result != -1);
		pagesize = (unsigned)result;

		/*
		 * We assume that pagesize is a power of 2 when calculating
		 * pagesize_mask and lg_pagesize.
		 */
		assert(((result - 1) & result) == 0);
		pagesize_mask = result - 1;
		lg_pagesize = ffs((int)result) - 1;
	}
#endif

	for (i = 0; i < 3; i++) {
		unsigned j;

		/* Get runtime configuration. */
		switch (i) {
		case 0:
			if ((linklen = readlink("/etc/jemalloc.conf", buf,
						sizeof(buf) - 1)) != -1) {
				/*
				 * Use the contents of the "/etc/jemalloc.conf"
				 * symbolic link's name.
				 */
				buf[linklen] = '\0';
				opts = buf;
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 1:
			if ((opts = getenv("JEMALLOC_OPTIONS")) != NULL) {
				/*
				 * Do nothing; opts is already initialized to
				 * the value of the JEMALLOC_OPTIONS
				 * environment variable.
				 */
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 2:
			if (JEMALLOC_P(malloc_options) != NULL) {
				/*
				 * Use options that were compiled into the
				 * program.
				 */
				opts = JEMALLOC_P(malloc_options);
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		default:
			/* NOTREACHED */
			assert(false);
			buf[0] = '\0';
			opts = buf;
		}

		for (j = 0; opts[j] != '\0'; j++) {
			unsigned k, nreps;
			bool nseen;

			/* Parse repetition count, if any. */
			for (nreps = 0, nseen = false;; j++, nseen = true) {
				switch (opts[j]) {
					case '0': case '1': case '2': case '3':
					case '4': case '5': case '6': case '7':
					case '8': case '9':
						nreps *= 10;
						nreps += opts[j] - '0';
						break;
					default:
						goto MALLOC_OUT;
				}
			}
MALLOC_OUT:
			if (nseen == false)
				nreps = 1;

			for (k = 0; k < nreps; k++) {
				switch (opts[j]) {
				case 'a':
					opt_abort = false;
					break;
				case 'A':
					opt_abort = true;
					break;
#ifdef JEMALLOC_PROF
				case 'b':
					if (opt_lg_prof_bt_max > 0)
						opt_lg_prof_bt_max--;
					break;
				case 'B':
					if (opt_lg_prof_bt_max < LG_PROF_BT_MAX)
						opt_lg_prof_bt_max++;
					break;
#endif
				case 'c':
					if (opt_lg_cspace_max - 1 >
					    opt_lg_qspace_max &&
					    opt_lg_cspace_max >
					    LG_CACHELINE)
						opt_lg_cspace_max--;
					break;
				case 'C':
					if (opt_lg_cspace_max < PAGE_SHIFT
					    - 1)
						opt_lg_cspace_max++;
					break;
				case 'd':
					if (opt_lg_dirty_mult + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_dirty_mult++;
					break;
				case 'D':
					if (opt_lg_dirty_mult >= 0)
						opt_lg_dirty_mult--;
					break;
#ifdef JEMALLOC_PROF
				case 'e':
					opt_prof_active = false;
					break;
				case 'E':
					opt_prof_active = true;
					break;
				case 'f':
					opt_prof = false;
					break;
				case 'F':
					opt_prof = true;
					break;
#endif
#ifdef JEMALLOC_TCACHE
				case 'g':
					if (opt_lg_tcache_gc_sweep >= 0)
						opt_lg_tcache_gc_sweep--;
					break;
				case 'G':
					if (opt_lg_tcache_gc_sweep + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_tcache_gc_sweep++;
					break;
				case 'h':
					opt_tcache = false;
					break;
				case 'H':
					opt_tcache = true;
					break;
#endif
#ifdef JEMALLOC_PROF
				case 'i':
					if (opt_lg_prof_interval >= 0)
						opt_lg_prof_interval--;
					break;
				case 'I':
					if (opt_lg_prof_interval + 1 <
					    (sizeof(uint64_t) << 3))
						opt_lg_prof_interval++;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'j':
					opt_junk = false;
					break;
				case 'J':
					opt_junk = true;
					break;
#endif
				case 'k':
					/*
					 * Chunks always require at least one
					 * header page, plus one data page.
					 */
					if ((1U << (opt_lg_chunk - 1)) >=
					    (2U << PAGE_SHIFT))
						opt_lg_chunk--;
					break;
				case 'K':
					if (opt_lg_chunk + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_chunk++;
					break;
#ifdef JEMALLOC_PROF
				case 'l':
					opt_prof_leak = false;
					break;
				case 'L':
					opt_prof_leak = true;
					break;
#endif
#ifdef JEMALLOC_TCACHE
				case 'm':
					if (opt_lg_tcache_maxclass >= 0)
						opt_lg_tcache_maxclass--;
					break;
				case 'M':
					if (opt_lg_tcache_maxclass + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_tcache_maxclass++;
					break;
#endif
				case 'n':
					opt_narenas_lshift--;
					break;
				case 'N':
					opt_narenas_lshift++;
					break;
#ifdef JEMALLOC_SWAP
				case 'o':
					opt_overcommit = false;
					break;
				case 'O':
					opt_overcommit = true;
					break;
#endif
				case 'p':
					opt_stats_print = false;
					break;
				case 'P':
					opt_stats_print = true;
					break;
				case 'q':
					if (opt_lg_qspace_max > LG_QUANTUM)
						opt_lg_qspace_max--;
					break;
				case 'Q':
					if (opt_lg_qspace_max + 1 <
					    opt_lg_cspace_max)
						opt_lg_qspace_max++;
					break;
#ifdef JEMALLOC_PROF
				case 's':
					if (opt_lg_prof_sample > 0)
						opt_lg_prof_sample--;
					break;
				case 'S':
					if (opt_lg_prof_sample + 1 <
					    (sizeof(uint64_t) << 3))
						opt_lg_prof_sample++;
					break;
				case 'u':
					opt_prof_udump = false;
					break;
				case 'U':
					opt_prof_udump = true;
					break;
#endif
#ifdef JEMALLOC_SYSV
				case 'v':
					opt_sysv = false;
					break;
				case 'V':
					opt_sysv = true;
					break;
#endif
#ifdef JEMALLOC_XMALLOC
				case 'x':
					opt_xmalloc = false;
					break;
				case 'X':
					opt_xmalloc = true;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'z':
					opt_zero = false;
					break;
				case 'Z':
					opt_zero = true;
					break;
#endif
				default: {
					char cbuf[2];

					cbuf[0] = opts[j];
					cbuf[1] = '\0';
					malloc_write(
					    "<jemalloc>: Unsupported character "
					    "in malloc options: '");
					malloc_write(cbuf);
					malloc_write("'\n");
				}
				}
			}
		}
	}

	/* Register fork handlers. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork,
	    jemalloc_postfork) != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort)
			abort();
	}

	if (ctl_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort)
				abort();
		}
	}

	if (chunk_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (base_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef JEMALLOC_PROF
	prof_boot0();
#endif

	if (arena_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef JEMALLOC_TCACHE
	tcache_boot();
#endif

	if (huge_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas = 1;
	arenas = init_arenas;
	memset(arenas, 0, sizeof(arena_t *) * narenas);

	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * choose_arena_hard().
	 */
	arenas_extend(0);
	if (arenas[0] == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifndef NO_TLS
	/*
	 * Assign the initial arena to the initial thread, in order to avoid
	 * spurious creation of an extra arena if the application switches to
	 * threaded mode.
	 */
	arenas_map = arenas[0];
#endif

	malloc_mutex_init(&arenas_lock);

#ifdef JEMALLOC_PROF
	if (prof_boot1()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
#endif

	/* Get number of CPUs. */
	malloc_initializer = pthread_self();
	malloc_mutex_unlock(&init_lock);
	ncpus = malloc_ncpus();
	malloc_mutex_lock(&init_lock);

	if (ncpus > 1) {
		/*
		 * For SMP systems, create more than one arena per CPU by
		 * default.
		 */
		opt_narenas_lshift += 2;
	}

	/* Determine how many arenas to use. */
	narenas = ncpus;
	if (opt_narenas_lshift > 0) {
		if ((narenas << opt_narenas_lshift) > narenas)
			narenas <<= opt_narenas_lshift;
		/*
		 * Make sure not to exceed the limits of what base_alloc() can
		 * handle.
		 */
		if (narenas * sizeof(arena_t *) > chunksize)
			narenas = chunksize / sizeof(arena_t *);
	} else if (opt_narenas_lshift < 0) {
		if ((narenas >> -opt_narenas_lshift) < narenas)
			narenas >>= -opt_narenas_lshift;
		/* Make sure there is at least one arena. */
		if (narenas == 0)
			narenas = 1;
	}

#ifdef NO_TLS
	if (narenas > 1) {
		static const unsigned primes[] = {1, 3, 5, 7, 11, 13, 17, 19,
		    23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
		    89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149,
		    151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211,
		    223, 227, 229, 233, 239, 241, 251, 257, 263};
		unsigned nprimes, parenas;

		/*
		 * Pick a prime number of hash arenas that is more than narenas
		 * so that direct hashing of pthread_self() pointers tends to
		 * spread allocations evenly among the arenas.
		 */
		assert((narenas & 1) == 0); /* narenas must be even. */
		nprimes = (sizeof(primes) >> LG_SIZEOF_INT);
		parenas = primes[nprimes - 1]; /* In case not enough primes. */
		for (i = 1; i < nprimes; i++) {
			if (primes[i] > narenas) {
				parenas = primes[i];
				break;
			}
		}
		narenas = parenas;
	}
#endif

#ifndef NO_TLS
	next_arena = 0;
#endif

	/* Allocate and initialize arenas. */
	arenas = (arena_t **)base_alloc(sizeof(arena_t *) * narenas);
	if (arenas == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	/*
	 * Zero the array.  In practice, this should always be pre-zeroed,
	 * since it was just mmap()ed, but let's be sure.
	 */
	memset(arenas, 0, sizeof(arena_t *) * narenas);
	/* Copy the pointer to the one arena that was already initialized. */
	arenas[0] = init_arenas[0];

	malloc_initialized = true;
	malloc_mutex_unlock(&init_lock);
	return (false);
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(malloc)(size_t size)
{
	void *ret;
#ifdef JEMALLOC_PROF
	prof_thr_cnt_t *cnt;
#endif

	if (malloc_init()) {
		ret = NULL;
		goto OOM;
	}

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
#  ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write("<jemalloc>: Error in malloc(): "
				    "invalid size 0\n");
				abort();
			}
#  endif
			ret = NULL;
			goto RETURN;
		}
#endif
	}

#ifdef JEMALLOC_PROF
	if (opt_prof) {
		if ((cnt = prof_alloc_prep(size)) == NULL) {
			ret = NULL;
			goto OOM;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && size <=
		    small_maxclass) {
			ret = imalloc(small_maxclass+1);
			if (ret != NULL)
				arena_prof_promoted(ret, size);
		} else
			ret = imalloc(size);
	} else
#endif
		ret = imalloc(size);

OOM:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write("<jemalloc>: Error in malloc(): "
			    "out of memory\n");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_PROF
	if (opt_prof && ret != NULL)
		prof_malloc(ret, cnt);
#endif
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(posix_memalign)(void **memptr, size_t alignment, size_t size)
{
	int ret;
	void *result;
#ifdef JEMALLOC_PROF
	prof_thr_cnt_t *cnt;
#endif

	if (malloc_init())
		result = NULL;
	else {
		if (size == 0) {
#ifdef JEMALLOC_SYSV
			if (opt_sysv == false)
#endif
				size = 1;
#ifdef JEMALLOC_SYSV
			else {
#  ifdef JEMALLOC_XMALLOC
				if (opt_xmalloc) {
					malloc_write("<jemalloc>: Error in "
					    "posix_memalign(): invalid size "
					    "0\n");
					abort();
				}
#  endif
				result = NULL;
				*memptr = NULL;
				ret = 0;
				goto RETURN;
			}
#endif
		}

		/* Make sure that alignment is a large enough power of 2. */
		if (((alignment - 1) & alignment) != 0
		    || alignment < sizeof(void *)) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write("<jemalloc>: Error in "
				    "posix_memalign(): invalid alignment\n");
				abort();
			}
#endif
			result = NULL;
			ret = EINVAL;
			goto RETURN;
		}

#ifdef JEMALLOC_PROF
		if (opt_prof) {
			if ((cnt = prof_alloc_prep(size)) == NULL) {
				result = NULL;
				ret = EINVAL;
			} else {
				if (prof_promote && (uintptr_t)cnt !=
				    (uintptr_t)1U && size <= small_maxclass) {
					result = ipalloc(alignment,
					    small_maxclass+1);
					if (result != NULL) {
						arena_prof_promoted(result,
						    size);
					}
				} else
					result = ipalloc(alignment, size);
			}
		} else
#endif
			result = ipalloc(alignment, size);
	}

	if (result == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write("<jemalloc>: Error in posix_memalign(): "
			    "out of memory\n");
			abort();
		}
#endif
		ret = ENOMEM;
		goto RETURN;
	}

	*memptr = result;
	ret = 0;

RETURN:
#ifdef JEMALLOC_PROF
	if (opt_prof && result != NULL)
		prof_malloc(result, cnt);
#endif
	return (ret);
}

JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(calloc)(size_t num, size_t size)
{
	void *ret;
	size_t num_size;
#ifdef JEMALLOC_PROF
	prof_thr_cnt_t *cnt;
#endif

	if (malloc_init()) {
		num_size = 0;
		ret = NULL;
		goto RETURN;
	}

	num_size = num * size;
	if (num_size == 0) {
#ifdef JEMALLOC_SYSV
		if ((opt_sysv == false) && ((num == 0) || (size == 0)))
#endif
			num_size = 1;
#ifdef JEMALLOC_SYSV
		else {
			ret = NULL;
			goto RETURN;
		}
#endif
	/*
	 * Try to avoid division here.  We know that it isn't possible to
	 * overflow during multiplication if neither operand uses any of the
	 * most significant half of the bits in a size_t.
	 */
	} else if (((num | size) & (SIZE_T_MAX << (sizeof(size_t) << 2)))
	    && (num_size / size != num)) {
		/* size_t overflow. */
		ret = NULL;
		goto RETURN;
	}

#ifdef JEMALLOC_PROF
	if (opt_prof) {
		if ((cnt = prof_alloc_prep(num_size)) == NULL) {
			ret = NULL;
			goto RETURN;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && num_size
		    <= small_maxclass) {
			ret = icalloc(small_maxclass+1);
			if (ret != NULL)
				arena_prof_promoted(ret, num_size);
		} else
			ret = icalloc(num_size);
	} else
#endif
		ret = icalloc(num_size);

RETURN:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write("<jemalloc>: Error in calloc(): out of "
			    "memory\n");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_PROF
	if (opt_prof && ret != NULL)
		prof_malloc(ret, cnt);
#endif
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(realloc)(void *ptr, size_t size)
{
	void *ret;
#ifdef JEMALLOC_PROF
	size_t old_size;
	prof_thr_cnt_t *cnt, *old_cnt;
#endif

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
			if (ptr != NULL) {
#ifdef JEMALLOC_PROF
				if (opt_prof) {
					old_size = isalloc(ptr);
					old_cnt = prof_cnt_get(ptr);
					cnt = NULL;
				}
#endif
				idalloc(ptr);
			}
#ifdef JEMALLOC_PROF
			else if (opt_prof) {
				old_size = 0;
				old_cnt = NULL;
				cnt = NULL;
			}
#endif
			ret = NULL;
			goto RETURN;
		}
#endif
	}

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_PROF
		if (opt_prof) {
			old_size = isalloc(ptr);
			old_cnt = prof_cnt_get(ptr);
			if ((cnt = prof_alloc_prep(size)) == NULL) {
				ret = NULL;
				goto OOM;
			}
			if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U &&
			    size <= small_maxclass) {
				ret = iralloc(ptr, small_maxclass+1);
				if (ret != NULL)
					arena_prof_promoted(ret, size);
			} else
				ret = iralloc(ptr, size);
		} else
#endif
			ret = iralloc(ptr, size);

#ifdef JEMALLOC_PROF
OOM:
#endif
		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	} else {
#ifdef JEMALLOC_PROF
		if (opt_prof) {
			old_size = 0;
			old_cnt = NULL;
		}
#endif
		if (malloc_init()) {
#ifdef JEMALLOC_PROF
			if (opt_prof)
				cnt = NULL;
#endif
			ret = NULL;
		} else {
#ifdef JEMALLOC_PROF
			if (opt_prof) {
				if ((cnt = prof_alloc_prep(size)) == NULL)
					ret = NULL;
				else {
					if (prof_promote && (uintptr_t)cnt !=
					    (uintptr_t)1U && size <=
					    small_maxclass) {
						ret = imalloc(small_maxclass+1);
						if (ret != NULL) {
							arena_prof_promoted(ret,
							    size);
						}
					} else
						ret = imalloc(size);
				}
			} else
#endif
				ret = imalloc(size);
		}

		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_PROF
	if (opt_prof)
		prof_realloc(ret, cnt, ptr, old_size, old_cnt);
#endif
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(free)(void *ptr)
{

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_PROF
		if (opt_prof)
			prof_free(ptr);
#endif
		idalloc(ptr);
	}
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

JEMALLOC_ATTR(visibility("default"))
size_t
JEMALLOC_P(malloc_usable_size)(const void *ptr)
{
	size_t ret;

	assert(ptr != NULL);
	ret = isalloc(ptr);

	return (ret);
}

#ifdef JEMALLOC_SWAP
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(malloc_swap_enable)(const int *fds, unsigned nfds, int prezeroed)
{

	/*
	 * Make sure malloc is initialized, because we need page size, chunk
	 * size, etc.
	 */
	if (malloc_init())
		return (-1);

	return (chunk_swap_enable(fds, nfds, (prezeroed != 0)) ? -1 : 0);
}
#endif

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(malloc_stats_print)(void (*write_cb)(void *, const char *),
    void *cbopaque, const char *opts)
{

	stats_print(write_cb, cbopaque, opts);
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctl)(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_byname(name, oldp, oldlenp, newp, newlen));
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctlnametomib)(const char *name, size_t *mibp, size_t *miblenp)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_nametomib(name, mibp, miblenp));
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctlbymib)(const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_bymib(mib, miblen, oldp, oldlenp, newp, newlen));
}

/*
 * End non-standard functions.
 */
/******************************************************************************/

/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().  These functions are only called if the program is
 * running in threaded mode, so there is no need to check whether the program
 * is threaded here.
 */

static void
jemalloc_prefork(void)
{
	unsigned i;

	/* Acquire all mutexes in a safe order. */

	malloc_mutex_lock(&arenas_lock);
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			malloc_mutex_lock(&arenas[i]->lock);
	}

	malloc_mutex_lock(&base_mtx);

	malloc_mutex_lock(&huge_mtx);

#ifdef JEMALLOC_DSS
	malloc_mutex_lock(&dss_mtx);
#endif

#ifdef JEMALLOC_SWAP
	malloc_mutex_lock(&swap_mtx);
#endif
}

static void
jemalloc_postfork(void)
{
	unsigned i;

	/* Release all mutexes, now that fork() has completed. */

#ifdef JEMALLOC_SWAP
	malloc_mutex_unlock(&swap_mtx);
#endif

#ifdef JEMALLOC_DSS
	malloc_mutex_unlock(&dss_mtx);
#endif

	malloc_mutex_unlock(&huge_mtx);

	malloc_mutex_unlock(&base_mtx);

	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			malloc_mutex_unlock(&arenas[i]->lock);
	}
	malloc_mutex_unlock(&arenas_lock);
}
