#define	JEMALLOC_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

malloc_mutex_t		arenas_lock;
arena_t			**arenas;
unsigned		narenas;

pthread_key_t		arenas_tsd;
#ifndef NO_TLS
__thread arena_t	*arenas_tls JEMALLOC_ATTR(tls_model("initial-exec"));
#endif

#ifndef NO_TLS
__thread thread_allocated_t	thread_allocated_tls;
#endif
pthread_key_t		thread_allocated_tsd;

/* Set to true once the allocator has been initialized. */
static bool		malloc_initialized = false;

/* Used to let the initializing thread recursively allocate. */
static pthread_t	malloc_initializer = (unsigned long)0;

/* Used to avoid initialization races. */
static malloc_mutex_t	init_lock = MALLOC_MUTEX_INITIALIZER;

#ifdef DYNAMIC_PAGE_SHIFT
size_t		pagesize;
size_t		pagesize_mask;
size_t		lg_pagesize;
#endif

unsigned	ncpus;

/* Runtime configuration options. */
const char	*JEMALLOC_P(malloc_conf) JEMALLOC_ATTR(visibility("default"));
#ifdef JEMALLOC_DEBUG
bool	opt_abort = true;
#  ifdef JEMALLOC_FILL
bool	opt_junk = true;
#  else
bool	opt_junk = false;
#  endif
#else
bool	opt_abort = false;
bool	opt_junk = false;
#endif
bool	opt_sysv = false;
bool	opt_xmalloc = false;
bool	opt_zero = false;
size_t	opt_narenas = 0;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	wrtmessage(void *cbopaque, const char *s);
static void	stats_print_atexit(void);
static unsigned	malloc_ncpus(void);
static void	arenas_cleanup(void *arg);
#ifdef NO_TLS
static void	thread_allocated_cleanup(void *arg);
#endif
static bool	malloc_conf_next(char const **opts_p, char const **k_p,
    size_t *klen_p, char const **v_p, size_t *vlen_p);
static void	malloc_conf_error(const char *msg, const char *k, size_t klen,
    const char *v, size_t vlen);
static void	malloc_conf_init(void);
static bool	malloc_init_hard(void);
static int	imemalign(void **memptr, size_t alignment, size_t size);

/******************************************************************************/
/* malloc_message() setup. */

JEMALLOC_CATTR(visibility("hidden"), static)
void
wrtmessage(void *cbopaque, const char *s)
{
	UNUSED int result = write(STDERR_FILENO, s, strlen(s));
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
	ret = (arena_t *)base_alloc(offsetof(arena_t, bins)
	    + (sizeof(arena_bin_t) * nbins));
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

/*
 * Choose an arena based on a per-thread value (slow-path code only, called
 * only by choose_arena()).
 */
arena_t *
choose_arena_hard(void)
{
	arena_t *ret;

	if (narenas > 1) {
		unsigned i, choose, first_null;

		choose = 0;
		first_null = narenas;
		malloc_mutex_lock(&arenas_lock);
		assert(arenas[0] != NULL);
		for (i = 1; i < narenas; i++) {
			if (arenas[i] != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				if (arenas[i]->nthreads <
				    arenas[choose]->nthreads)
					choose = i;
			} else if (first_null == narenas) {
				/*
				 * Record the index of the first uninitialized
				 * arena, in case all extant arenas are in use.
				 *
				 * NB: It is possible for there to be
				 * discontinuities in terms of initialized
				 * versus uninitialized arenas, due to the
				 * "thread.arena" mallctl.
				 */
				first_null = i;
			}
		}

		if (arenas[choose] == 0 || first_null == narenas) {
			/*
			 * Use an unloaded arena, or the least loaded arena if
			 * all arenas are already initialized.
			 */
			ret = arenas[choose];
		} else {
			/* Initialize a new arena. */
			ret = arenas_extend(first_null);
		}
		ret->nthreads++;
		malloc_mutex_unlock(&arenas_lock);
	} else {
		ret = arenas[0];
		malloc_mutex_lock(&arenas_lock);
		ret->nthreads++;
		malloc_mutex_unlock(&arenas_lock);
	}

	ARENA_SET(ret);

	return (ret);
}

/*
 * glibc provides a non-standard strerror_r() when _GNU_SOURCE is defined, so
 * provide a wrapper.
 */
int
buferror(int errnum, char *buf, size_t buflen)
{
#ifdef _GNU_SOURCE
	char *b = strerror_r(errno, buf, buflen);
	if (b != buf) {
		strncpy(buf, b, buflen);
		buf[buflen-1] = '\0';
	}
	return (0);
#else
	return (strerror_r(errno, buf, buflen));
#endif
}

static void
stats_print_atexit(void)
{

	if (config_tcache && config_stats) {
		unsigned i;

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0; i < narenas; i++) {
			arena_t *arena = arenas[i];
			if (arena != NULL) {
				tcache_t *tcache;

				/*
				 * tcache_stats_merge() locks bins, so if any
				 * code is introduced that acquires both arena
				 * and bin locks in the opposite order,
				 * deadlocks may result.
				 */
				malloc_mutex_lock(&arena->lock);
				ql_foreach(tcache, &arena->tcache_ql, link) {
					tcache_stats_merge(tcache, arena);
				}
				malloc_mutex_unlock(&arena->lock);
			}
		}
	}
	JEMALLOC_P(malloc_stats_print)(NULL, NULL, NULL);
}

thread_allocated_t *
thread_allocated_get_hard(void)
{
	thread_allocated_t *thread_allocated = (thread_allocated_t *)
	    imalloc(sizeof(thread_allocated_t));
	if (thread_allocated == NULL) {
		static thread_allocated_t static_thread_allocated = {0, 0};
		malloc_write("<jemalloc>: Error allocating TSD;"
		    " mallctl(\"thread.{de,}allocated[p]\", ...)"
		    " will be inaccurate\n");
		if (opt_abort)
			abort();
		return (&static_thread_allocated);
	}
	pthread_setspecific(thread_allocated_tsd, thread_allocated);
	thread_allocated->allocated = 0;
	thread_allocated->deallocated = 0;
	return (thread_allocated);
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

static void
arenas_cleanup(void *arg)
{
	arena_t *arena = (arena_t *)arg;

	malloc_mutex_lock(&arenas_lock);
	arena->nthreads--;
	malloc_mutex_unlock(&arenas_lock);
}

#ifdef NO_TLS
static void
thread_allocated_cleanup(void *arg)
{
	uint64_t *allocated = (uint64_t *)arg;

	if (allocated != NULL)
		idalloc(allocated);
}
#endif

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
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p)
{
	bool accept;
	const char *opts = *opts_p;

	*k_p = opts;

	for (accept = false; accept == false;) {
		switch (*opts) {
			case 'A': case 'B': case 'C': case 'D': case 'E':
			case 'F': case 'G': case 'H': case 'I': case 'J':
			case 'K': case 'L': case 'M': case 'N': case 'O':
			case 'P': case 'Q': case 'R': case 'S': case 'T':
			case 'U': case 'V': case 'W': case 'X': case 'Y':
			case 'Z':
			case 'a': case 'b': case 'c': case 'd': case 'e':
			case 'f': case 'g': case 'h': case 'i': case 'j':
			case 'k': case 'l': case 'm': case 'n': case 'o':
			case 'p': case 'q': case 'r': case 's': case 't':
			case 'u': case 'v': case 'w': case 'x': case 'y':
			case 'z':
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case '_':
				opts++;
				break;
			case ':':
				opts++;
				*klen_p = (uintptr_t)opts - 1 - (uintptr_t)*k_p;
				*v_p = opts;
				accept = true;
				break;
			case '\0':
				if (opts != *opts_p) {
					malloc_write("<jemalloc>: Conf string "
					    "ends with key\n");
				}
				return (true);
			default:
				malloc_write("<jemalloc>: Malformed conf "
				    "string\n");
				return (true);
		}
	}

	for (accept = false; accept == false;) {
		switch (*opts) {
			case ',':
				opts++;
				/*
				 * Look ahead one character here, because the
				 * next time this function is called, it will
				 * assume that end of input has been cleanly
				 * reached if no input remains, but we have
				 * optimistically already consumed the comma if
				 * one exists.
				 */
				if (*opts == '\0') {
					malloc_write("<jemalloc>: Conf string "
					    "ends with comma\n");
				}
				*vlen_p = (uintptr_t)opts - 1 - (uintptr_t)*v_p;
				accept = true;
				break;
			case '\0':
				*vlen_p = (uintptr_t)opts - (uintptr_t)*v_p;
				accept = true;
				break;
			default:
				opts++;
				break;
		}
	}

	*opts_p = opts;
	return (false);
}

static void
malloc_conf_error(const char *msg, const char *k, size_t klen, const char *v,
    size_t vlen)
{
	char buf[PATH_MAX + 1];

	malloc_write("<jemalloc>: ");
	malloc_write(msg);
	malloc_write(": ");
	memcpy(buf, k, klen);
	memcpy(&buf[klen], ":", 1);
	memcpy(&buf[klen+1], v, vlen);
	buf[klen+1+vlen] = '\0';
	malloc_write(buf);
	malloc_write("\n");
}

static void
malloc_conf_init(void)
{
	unsigned i;
	char buf[PATH_MAX + 1];
	const char *opts, *k, *v;
	size_t klen, vlen;

	for (i = 0; i < 3; i++) {
		/* Get runtime configuration. */
		switch (i) {
		case 0:
			if (JEMALLOC_P(malloc_conf) != NULL) {
				/*
				 * Use options that were compiled into the
				 * program.
				 */
				opts = JEMALLOC_P(malloc_conf);
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 1: {
			int linklen;
			const char *linkname =
#ifdef JEMALLOC_PREFIX
			    "/etc/"JEMALLOC_PREFIX"malloc.conf"
#else
			    "/etc/malloc.conf"
#endif
			    ;

			if ((linklen = readlink(linkname, buf,
			    sizeof(buf) - 1)) != -1) {
				/*
				 * Use the contents of the "/etc/malloc.conf"
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
		}
		case 2: {
			const char *envname =
#ifdef JEMALLOC_PREFIX
			    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
			    "MALLOC_CONF"
#endif
			    ;

			if ((opts = getenv(envname)) != NULL) {
				/*
				 * Do nothing; opts is already initialized to
				 * the value of the MALLOC_CONF environment
				 * variable.
				 */
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		}
		default:
			/* NOTREACHED */
			assert(false);
			buf[0] = '\0';
			opts = buf;
		}

		while (*opts != '\0' && malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen) == false) {
#define	CONF_HANDLE_BOOL(n)						\
			if (sizeof(#n)-1 == klen && strncmp(#n, k,	\
			    klen) == 0) {				\
				if (strncmp("true", v, vlen) == 0 &&	\
				    vlen == sizeof("true")-1)		\
					opt_##n = true;			\
				else if (strncmp("false", v, vlen) ==	\
				    0 && vlen == sizeof("false")-1)	\
					opt_##n = false;		\
				else {					\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				}					\
				continue;				\
			}
#define	CONF_HANDLE_SIZE_T(n, min, max)					\
			if (sizeof(#n)-1 == klen && strncmp(#n, k,	\
			    klen) == 0) {				\
				unsigned long ul;			\
				char *end;				\
									\
				errno = 0;				\
				ul = strtoul(v, &end, 0);		\
				if (errno != 0 || (uintptr_t)end -	\
				    (uintptr_t)v != vlen) {		\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				} else if (ul < min || ul > max) {	\
					malloc_conf_error(		\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else					\
					opt_##n = ul;			\
				continue;				\
			}
#define	CONF_HANDLE_SSIZE_T(n, min, max)				\
			if (sizeof(#n)-1 == klen && strncmp(#n, k,	\
			    klen) == 0) {				\
				long l;					\
				char *end;				\
									\
				errno = 0;				\
				l = strtol(v, &end, 0);			\
				if (errno != 0 || (uintptr_t)end -	\
				    (uintptr_t)v != vlen) {		\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				} else if (l < (ssize_t)min || l >	\
				    (ssize_t)max) {			\
					malloc_conf_error(		\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else					\
					opt_##n = l;			\
				continue;				\
			}
#define	CONF_HANDLE_CHAR_P(n, d)					\
			if (sizeof(#n)-1 == klen && strncmp(#n, k,	\
			    klen) == 0) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(opt_##n)-1) ? vlen :		\
				    sizeof(opt_##n)-1;			\
				strncpy(opt_##n, v, cpylen);		\
				opt_##n[cpylen] = '\0';			\
				continue;				\
			}

			CONF_HANDLE_BOOL(abort)
			CONF_HANDLE_SIZE_T(lg_qspace_max, LG_QUANTUM,
			    PAGE_SHIFT-1)
			CONF_HANDLE_SIZE_T(lg_cspace_max, LG_QUANTUM,
			    PAGE_SHIFT-1)
			/*
			 * Chunks always require at least one * header page,
			 * plus one data page.
			 */
			CONF_HANDLE_SIZE_T(lg_chunk, PAGE_SHIFT+1,
			    (sizeof(size_t) << 3) - 1)
			CONF_HANDLE_SIZE_T(narenas, 1, SIZE_T_MAX)
			CONF_HANDLE_SSIZE_T(lg_dirty_mult, -1,
			    (sizeof(size_t) << 3) - 1)
			CONF_HANDLE_BOOL(stats_print)
			if (config_fill) {
				CONF_HANDLE_BOOL(junk)
				CONF_HANDLE_BOOL(zero)
			}
			if (config_sysv) {
				CONF_HANDLE_BOOL(sysv)
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(xmalloc)
			}
			if (config_tcache) {
				CONF_HANDLE_BOOL(tcache)
				CONF_HANDLE_SSIZE_T(lg_tcache_gc_sweep, -1,
				    (sizeof(size_t) << 3) - 1)
				CONF_HANDLE_SSIZE_T(lg_tcache_max, -1,
				    (sizeof(size_t) << 3) - 1)
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(prof)
				CONF_HANDLE_CHAR_P(prof_prefix, "jeprof")
				CONF_HANDLE_BOOL(prof_active)
				CONF_HANDLE_SSIZE_T(lg_prof_sample, 0,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(prof_accum)
				CONF_HANDLE_SSIZE_T(lg_prof_interval, -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(prof_gdump)
				CONF_HANDLE_BOOL(prof_leak)
			}
			malloc_conf_error("Invalid conf pair", k, klen, v,
			    vlen);
#undef CONF_HANDLE_BOOL
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
		}

		/* Validate configuration of options that are inter-related. */
		if (opt_lg_qspace_max+1 >= opt_lg_cspace_max) {
			malloc_write("<jemalloc>: Invalid lg_[qc]space_max "
			    "relationship; restoring defaults\n");
			opt_lg_qspace_max = LG_QSPACE_MAX_DEFAULT;
			opt_lg_cspace_max = LG_CSPACE_MAX_DEFAULT;
		}
	}
}

static bool
malloc_init_hard(void)
{
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
		malloc_mutex_unlock(&init_lock);
		return (false);
	}

#ifdef DYNAMIC_PAGE_SHIFT
	/* Get page size. */
	{
		long result;

		result = sysconf(_SC_PAGESIZE);
		assert(result != -1);
		pagesize = (size_t)result;

		/*
		 * We assume that pagesize is a power of 2 when calculating
		 * pagesize_mask and lg_pagesize.
		 */
		assert(((result - 1) & result) == 0);
		pagesize_mask = result - 1;
		lg_pagesize = ffs((int)result) - 1;
	}
#endif

	if (config_prof)
		prof_boot0();

	malloc_conf_init();

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

	if (config_prof)
		prof_boot1();

	if (arena_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (config_tcache && tcache_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (huge_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef NO_TLS
	/* Initialize allocation counters before any allocations can occur. */
	if (config_stats && pthread_key_create(&thread_allocated_tsd,
	    thread_allocated_cleanup) != 0) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
#endif

	if (malloc_mutex_init(&arenas_lock))
		return (true);

	if (pthread_key_create(&arenas_tsd, arenas_cleanup) != 0) {
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

	/*
	 * Assign the initial arena to the initial thread, in order to avoid
	 * spurious creation of an extra arena if the application switches to
	 * threaded mode.
	 */
	ARENA_SET(arenas[0]);
	arenas[0]->nthreads++;

	if (config_prof && prof_boot2()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/* Get number of CPUs. */
	malloc_initializer = pthread_self();
	malloc_mutex_unlock(&init_lock);
	ncpus = malloc_ncpus();
	malloc_mutex_lock(&init_lock);

	if (opt_narenas == 0) {
		/*
		 * For SMP systems, create more than one arena per CPU by
		 * default.
		 */
		if (ncpus > 1)
			opt_narenas = ncpus << 2;
		else
			opt_narenas = 1;
	}
	narenas = opt_narenas;
	/*
	 * Make sure that the arenas array can be allocated.  In practice, this
	 * limit is enough to allow the allocator to function, but the ctl
	 * machinery will fail to allocate memory at far lower limits.
	 */
	if (narenas > chunksize / sizeof(arena_t *)) {
		char buf[UMAX2S_BUFSIZE];

		narenas = chunksize / sizeof(arena_t *);
		malloc_write("<jemalloc>: Reducing narenas to limit (");
		malloc_write(u2s(narenas, 10, buf));
		malloc_write(")\n");
	}

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

#ifdef JEMALLOC_ZONE
	/* Register the custom zone. */
	malloc_zone_register(create_zone());

	/*
	 * Convert the default szone to an "overlay zone" that is capable of
	 * deallocating szone-allocated objects, but allocating new objects
	 * from jemalloc.
	 */
	szone2ozone(malloc_default_zone());
#endif

	malloc_initialized = true;
	malloc_mutex_unlock(&init_lock);
	return (false);
}

#ifdef JEMALLOC_ZONE
JEMALLOC_ATTR(constructor)
void
jemalloc_darwin_init(void)
{

	if (malloc_init_hard())
		abort();
}
#endif

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
	size_t usize;
	prof_thr_cnt_t *cnt
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;

	if (malloc_init()) {
		ret = NULL;
		goto OOM;
	}

	if (size == 0) {
		if (config_sysv == false || opt_sysv == false)
			size = 1;
		else {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in malloc(): "
				    "invalid size 0\n");
				abort();
			}
			ret = NULL;
			goto RETURN;
		}
	}

	if (config_prof && opt_prof) {
		usize = s2u(size);
		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL) {
			ret = NULL;
			goto OOM;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize <=
		    small_maxclass) {
			ret = imalloc(small_maxclass+1);
			if (ret != NULL)
				arena_prof_promoted(ret, usize);
		} else
			ret = imalloc(size);
	} else {
		if (config_stats)
			usize = s2u(size);
		ret = imalloc(size);
	}

OOM:
	if (ret == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error in malloc(): "
			    "out of memory\n");
			abort();
		}
		errno = ENOMEM;
	}

RETURN:
	if (config_prof && opt_prof && ret != NULL)
		prof_malloc(ret, usize, cnt);
	if (config_stats && ret != NULL) {
		assert(usize == isalloc(ret));
		ALLOCATED_ADD(usize, 0);
	}
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
#ifdef JEMALLOC_PROF
/*
 * Avoid any uncertainty as to how many backtrace frames to ignore in
 * PROF_ALLOC_PREP().
 */
JEMALLOC_ATTR(noinline)
#endif
static int
imemalign(void **memptr, size_t alignment, size_t size)
{
	int ret;
	size_t usize;
	void *result;
	prof_thr_cnt_t *cnt
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;

	if (malloc_init())
		result = NULL;
	else {
		if (size == 0) {
			if (config_sysv == false || opt_sysv == false)
				size = 1;
			else {
				if (config_xmalloc && opt_xmalloc) {
					malloc_write("<jemalloc>: Error in "
					    "posix_memalign(): invalid size "
					    "0\n");
					abort();
				}
				result = NULL;
				*memptr = NULL;
				ret = 0;
				goto RETURN;
			}
		}

		/* Make sure that alignment is a large enough power of 2. */
		if (((alignment - 1) & alignment) != 0
		    || alignment < sizeof(void *)) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in "
				    "posix_memalign(): invalid alignment\n");
				abort();
			}
			result = NULL;
			ret = EINVAL;
			goto RETURN;
		}

		usize = sa2u(size, alignment, NULL);
		if (usize == 0) {
			result = NULL;
			ret = ENOMEM;
			goto RETURN;
		}

		if (config_prof && opt_prof) {
			PROF_ALLOC_PREP(2, usize, cnt);
			if (cnt == NULL) {
				result = NULL;
				ret = EINVAL;
			} else {
				if (prof_promote && (uintptr_t)cnt !=
				    (uintptr_t)1U && usize <= small_maxclass) {
					assert(sa2u(small_maxclass+1,
					    alignment, NULL) != 0);
					result = ipalloc(sa2u(small_maxclass+1,
					    alignment, NULL), alignment, false);
					if (result != NULL) {
						arena_prof_promoted(result,
						    usize);
					}
				} else {
					result = ipalloc(usize, alignment,
					    false);
				}
			}
		} else
			result = ipalloc(usize, alignment, false);
	}

	if (result == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error in posix_memalign(): "
			    "out of memory\n");
			abort();
		}
		ret = ENOMEM;
		goto RETURN;
	}

	*memptr = result;
	ret = 0;

RETURN:
	if (config_stats && result != NULL) {
		assert(usize == isalloc(result));
		ALLOCATED_ADD(usize, 0);
	}
	if (config_prof && opt_prof && result != NULL)
		prof_malloc(result, usize, cnt);
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(posix_memalign)(void **memptr, size_t alignment, size_t size)
{

	return imemalign(memptr, alignment, size);
}

JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(calloc)(size_t num, size_t size)
{
	void *ret;
	size_t num_size;
	size_t usize;
	prof_thr_cnt_t *cnt
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;

	if (malloc_init()) {
		num_size = 0;
		ret = NULL;
		goto RETURN;
	}

	num_size = num * size;
	if (num_size == 0) {
		if ((config_sysv == false || opt_sysv == false)
		    && ((num == 0) || (size == 0)))
			num_size = 1;
		else {
			ret = NULL;
			goto RETURN;
		}
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

	if (config_prof && opt_prof) {
		usize = s2u(num_size);
		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL) {
			ret = NULL;
			goto RETURN;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize
		    <= small_maxclass) {
			ret = icalloc(small_maxclass+1);
			if (ret != NULL)
				arena_prof_promoted(ret, usize);
		} else
			ret = icalloc(num_size);
	} else {
		if (config_stats)
			usize = s2u(num_size);
		ret = icalloc(num_size);
	}

RETURN:
	if (ret == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error in calloc(): out of "
			    "memory\n");
			abort();
		}
		errno = ENOMEM;
	}

	if (config_prof && opt_prof && ret != NULL)
		prof_malloc(ret, usize, cnt);
	if (config_stats && ret != NULL) {
		assert(usize == isalloc(ret));
		ALLOCATED_ADD(usize, 0);
	}
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(realloc)(void *ptr, size_t size)
{
	void *ret;
	size_t usize;
	size_t old_size = 0;
	prof_thr_cnt_t *cnt
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;
	prof_ctx_t *old_ctx
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;

	if (size == 0) {
		if (config_sysv == false || opt_sysv == false)
			size = 1;
		else {
			if (ptr != NULL) {
				if (config_prof || config_stats)
					old_size = isalloc(ptr);
				if (config_prof && opt_prof) {
					old_ctx = prof_ctx_get(ptr);
					cnt = NULL;
				}
				idalloc(ptr);
			} else if (config_prof && opt_prof) {
				old_ctx = NULL;
				cnt = NULL;
			}
			ret = NULL;
			goto RETURN;
		}
	}

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

		if (config_prof || config_stats)
			old_size = isalloc(ptr);
		if (config_prof && opt_prof) {
			usize = s2u(size);
			old_ctx = prof_ctx_get(ptr);
			PROF_ALLOC_PREP(1, usize, cnt);
			if (cnt == NULL) {
				old_ctx = NULL;
				ret = NULL;
				goto OOM;
			}
			if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U &&
			    usize <= small_maxclass) {
				ret = iralloc(ptr, small_maxclass+1, 0, 0,
				    false, false);
				if (ret != NULL)
					arena_prof_promoted(ret, usize);
				else
					old_ctx = NULL;
			} else {
				ret = iralloc(ptr, size, 0, 0, false, false);
				if (ret == NULL)
					old_ctx = NULL;
			}
		} else {
			if (config_stats)
				usize = s2u(size);
			ret = iralloc(ptr, size, 0, 0, false, false);
		}

OOM:
		if (ret == NULL) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
			errno = ENOMEM;
		}
	} else {
		if (config_prof && opt_prof)
			old_ctx = NULL;
		if (malloc_init()) {
			if (config_prof && opt_prof)
				cnt = NULL;
			ret = NULL;
		} else {
			if (config_prof && opt_prof) {
				usize = s2u(size);
				PROF_ALLOC_PREP(1, usize, cnt);
				if (cnt == NULL)
					ret = NULL;
				else {
					if (prof_promote && (uintptr_t)cnt !=
					    (uintptr_t)1U && usize <=
					    small_maxclass) {
						ret = imalloc(small_maxclass+1);
						if (ret != NULL) {
							arena_prof_promoted(ret,
							    usize);
						}
					} else
						ret = imalloc(size);
				}
			} else {
				if (config_stats)
					usize = s2u(size);
				ret = imalloc(size);
			}
		}

		if (ret == NULL) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
			errno = ENOMEM;
		}
	}

RETURN:
	if (config_prof && opt_prof)
		prof_realloc(ret, usize, cnt, old_size, old_ctx);
	if (config_stats && ret != NULL) {
		assert(usize == isalloc(ret));
		ALLOCATED_ADD(usize, old_size);
	}
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(free)(void *ptr)
{

	if (ptr != NULL) {
		size_t usize;

		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

		if (config_prof && opt_prof) {
			usize = isalloc(ptr);
			prof_free(ptr, usize);
		} else if (config_stats) {
			usize = isalloc(ptr);
		}
		if (config_stats)
			ALLOCATED_ADD(0, usize);
		idalloc(ptr);
	}
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard override functions.
 *
 * These overrides are omitted if the JEMALLOC_PREFIX is defined, since the
 * entire point is to avoid accidental mixed allocator usage.
 */
#ifndef JEMALLOC_PREFIX

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(memalign)(size_t alignment, size_t size)
{
	void *ret
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;
	imemalign(&ret, alignment, size);
	return (ret);
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(valloc)(size_t size)
{
	void *ret
#ifdef JEMALLOC_CC_SILENCE
	    = NULL
#endif
	    ;
	imemalign(&ret, PAGE_SIZE, size);
	return (ret);
}
#endif

#endif /* JEMALLOC_PREFIX */
/*
 * End non-standard override functions.
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

	assert(malloc_initialized || malloc_initializer == pthread_self());

	if (config_ivsalloc)
		ret = ivsalloc(ptr);
	else {
		assert(ptr != NULL);
		ret = isalloc(ptr);
	}

	return (ret);
}

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

JEMALLOC_INLINE void *
iallocm(size_t usize, size_t alignment, bool zero)
{

	assert(usize == ((alignment == 0) ? s2u(usize) : sa2u(usize, alignment,
	    NULL)));

	if (alignment != 0)
		return (ipalloc(usize, alignment, zero));
	else if (zero)
		return (icalloc(usize));
	else
		return (imalloc(usize));
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(allocm)(void **ptr, size_t *rsize, size_t size, int flags)
{
	void *p;
	size_t usize;
	size_t alignment = (ZU(1) << (flags & ALLOCM_LG_ALIGN_MASK)
	    & (SIZE_T_MAX-1));
	bool zero = flags & ALLOCM_ZERO;
	prof_thr_cnt_t *cnt;

	assert(ptr != NULL);
	assert(size != 0);

	if (malloc_init())
		goto OOM;

	usize = (alignment == 0) ? s2u(size) : sa2u(size, alignment, NULL);
	if (usize == 0)
		goto OOM;

	if (config_prof && opt_prof) {
		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL)
			goto OOM;
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize <=
		    small_maxclass) {
			size_t usize_promoted = (alignment == 0) ?
			    s2u(small_maxclass+1) : sa2u(small_maxclass+1,
			    alignment, NULL);
			assert(usize_promoted != 0);
			p = iallocm(usize_promoted, alignment, zero);
			if (p == NULL)
				goto OOM;
			arena_prof_promoted(p, usize);
		} else {
			p = iallocm(usize, alignment, zero);
			if (p == NULL)
				goto OOM;
		}
		prof_malloc(p, usize, cnt);
	} else {
		p = iallocm(usize, alignment, zero);
		if (p == NULL)
			goto OOM;
	}
	if (rsize != NULL)
		*rsize = usize;

	*ptr = p;
	if (config_stats) {
		assert(usize == isalloc(p));
		ALLOCATED_ADD(usize, 0);
	}
	return (ALLOCM_SUCCESS);
OOM:
	if (config_xmalloc && opt_xmalloc) {
		malloc_write("<jemalloc>: Error in allocm(): "
		    "out of memory\n");
		abort();
	}
	*ptr = NULL;
	return (ALLOCM_ERR_OOM);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(rallocm)(void **ptr, size_t *rsize, size_t size, size_t extra,
    int flags)
{
	void *p, *q;
	size_t usize;
	size_t old_size;
	size_t alignment = (ZU(1) << (flags & ALLOCM_LG_ALIGN_MASK)
	    & (SIZE_T_MAX-1));
	bool zero = flags & ALLOCM_ZERO;
	bool no_move = flags & ALLOCM_NO_MOVE;
	prof_thr_cnt_t *cnt;

	assert(ptr != NULL);
	assert(*ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized || malloc_initializer == pthread_self());

	p = *ptr;
	if (config_prof && opt_prof) {
		/*
		 * usize isn't knowable before iralloc() returns when extra is
		 * non-zero.  Therefore, compute its maximum possible value and
		 * use that in PROF_ALLOC_PREP() to decide whether to capture a
		 * backtrace.  prof_realloc() will use the actual usize to
		 * decide whether to sample.
		 */
		size_t max_usize = (alignment == 0) ? s2u(size+extra) :
		    sa2u(size+extra, alignment, NULL);
		prof_ctx_t *old_ctx = prof_ctx_get(p);
		old_size = isalloc(p);
		PROF_ALLOC_PREP(1, max_usize, cnt);
		if (cnt == NULL)
			goto OOM;
		/*
		 * Use minimum usize to determine whether promotion may happen.
		 */
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U
		    && ((alignment == 0) ? s2u(size) : sa2u(size,
		    alignment, NULL)) <= small_maxclass) {
			q = iralloc(p, small_maxclass+1, (small_maxclass+1 >=
			    size+extra) ? 0 : size+extra - (small_maxclass+1),
			    alignment, zero, no_move);
			if (q == NULL)
				goto ERR;
			if (max_usize < PAGE_SIZE) {
				usize = max_usize;
				arena_prof_promoted(q, usize);
			} else
				usize = isalloc(q);
		} else {
			q = iralloc(p, size, extra, alignment, zero, no_move);
			if (q == NULL)
				goto ERR;
			usize = isalloc(q);
		}
		prof_realloc(q, usize, cnt, old_size, old_ctx);
		if (rsize != NULL)
			*rsize = usize;
	} else {
		if (config_stats)
			old_size = isalloc(p);
		q = iralloc(p, size, extra, alignment, zero, no_move);
		if (q == NULL)
			goto ERR;
		if (config_stats)
			usize = isalloc(q);
		if (rsize != NULL) {
			if (config_stats == false)
				usize = isalloc(q);
			*rsize = usize;
		}
	}

	*ptr = q;
	if (config_stats)
		ALLOCATED_ADD(usize, old_size);
	return (ALLOCM_SUCCESS);
ERR:
	if (no_move)
		return (ALLOCM_ERR_NOT_MOVED);
OOM:
	if (config_xmalloc && opt_xmalloc) {
		malloc_write("<jemalloc>: Error in rallocm(): "
		    "out of memory\n");
		abort();
	}
	return (ALLOCM_ERR_OOM);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(sallocm)(const void *ptr, size_t *rsize, int flags)
{
	size_t sz;

	assert(malloc_initialized || malloc_initializer == pthread_self());

	if (config_ivsalloc)
		sz = ivsalloc(ptr);
	else {
		assert(ptr != NULL);
		sz = isalloc(ptr);
	}
	assert(rsize != NULL);
	*rsize = sz;

	return (ALLOCM_SUCCESS);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(dallocm)(void *ptr, int flags)
{
	size_t usize;

	assert(ptr != NULL);
	assert(malloc_initialized || malloc_initializer == pthread_self());

	if (config_stats)
		usize = isalloc(ptr);
	if (config_prof && opt_prof) {
		if (config_stats == false)
			usize = isalloc(ptr);
		prof_free(ptr, usize);
	}
	if (config_stats)
		ALLOCATED_ADD(0, usize);
	idalloc(ptr);

	return (ALLOCM_SUCCESS);
}

/*
 * End non-standard functions.
 */
/******************************************************************************/

/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().
 */

void
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

	if (config_dss)
		malloc_mutex_lock(&dss_mtx);
}

void
jemalloc_postfork(void)
{
	unsigned i;

	/* Release all mutexes, now that fork() has completed. */

	if (config_dss)
		malloc_mutex_unlock(&dss_mtx);

	malloc_mutex_unlock(&huge_mtx);

	malloc_mutex_unlock(&base_mtx);

	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			malloc_mutex_unlock(&arenas[i]->lock);
	}
	malloc_mutex_unlock(&arenas_lock);
}

/******************************************************************************/
