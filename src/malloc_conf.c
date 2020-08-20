#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/malloc_conf.h"
#include "jemalloc/internal/log.h"

/* Number of sources for initializing malloc_conf */
#define MALLOC_CONF_NSOURCES 5

/* Runtime configuration options. */
const char *je_malloc_conf
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
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
bool opt_confirm_conf = false;
/* Whether encountered any invalid config options. */
static bool had_conf_error = false;

/*
 * The usual rule is that the closer to runtime you are, the higher priority
 * your configuration settings are (so the jemalloc config options get lower
 * priority than the per-binary setting, which gets lower priority than the /etc
 * setting, which gets lower priority than the environment settings).
 *
 * But it's a fairly common use case in some testing environments for a user to
 * be able to control the binary, but nothing else (e.g. a performancy canary
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

static char *
jemalloc_secure_getenv(const char *name) {
#ifdef JEMALLOC_HAVE_SECURE_GETENV
	return secure_getenv(name);
#else
#  ifdef JEMALLOC_HAVE_ISSETUGID
	if (issetugid() != 0) {
		return NULL;
	}
#  endif
	return getenv(name);
#endif
}

static void
init_opt_stats_opts(const char *v, size_t vlen, char *dest) {
	size_t opts_len = strlen(dest);
	assert(opts_len <= stats_print_tot_num_options);

	for (size_t i = 0; i < vlen; i++) {
		switch (v[i]) {
#define OPTION(o, v, d, s) case o: break;
			STATS_PRINT_OPTIONS
#undef OPTION
		default: continue;
		}

		if (strchr(dest, v[i]) != NULL) {
			/* Ignore repeated. */
			continue;
		}

		dest[opts_len++] = v[i];
		dest[opts_len] = '\0';
		assert(opts_len <= stats_print_tot_num_options);
	}
	assert(opts_len == strlen(dest));
}

/* Reads the next size pair in a multi-sized option. */
static bool
malloc_conf_multi_sizes_next(const char **slab_size_segment_cur,
    size_t *vlen_left, size_t *slab_start, size_t *slab_end, size_t *new_size) {
	const char *cur = *slab_size_segment_cur;
	char *end;
	uintmax_t um;

	set_errno(0);

	/* First number, then '-' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != '-') {
		return true;
	}
	*slab_start = (size_t)um;
	cur = end + 1;

	/* Second number, then ':' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != ':') {
		return true;
	}
	*slab_end = (size_t)um;
	cur = end + 1;

	/* Last number */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0) {
		return true;
	}
	*new_size = (size_t)um;

	/* Consume the separator if there is one. */
	if (*end == '|') {
		end++;
	}

	*vlen_left -= end - *slab_size_segment_cur;
	*slab_size_segment_cur = end;

	return false;
}

static bool
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p) {
	bool accept;
	const char *opts = *opts_p;

	*k_p = opts;

	for (accept = false; !accept;) {
		switch (*opts) {
		case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
		case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
		case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
		case 'Y': case 'Z':
		case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
		case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
		case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
		case 's': case 't': case 'u': case 'v': case 'w': case 'x':
		case 'y': case 'z':
		case '0': case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9':
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
				malloc_write("<jemalloc>: Conf string ends "
				    "with key\n");
			}
			return true;
		default:
			malloc_write("<jemalloc>: Malformed conf string\n");
			return true;
		}
	}

	for (accept = false; !accept;) {
		switch (*opts) {
		case ',':
			opts++;
			/*
			 * Look ahead one character here, because the next time
			 * this function is called, it will assume that end of
			 * input has been cleanly reached if no input remains,
			 * but we have optimistically already consumed the
			 * comma if one exists.
			 */
			if (*opts == '\0') {
				malloc_write("<jemalloc>: Conf string ends "
				    "with comma\n");
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
	return false;
}

static void
malloc_abort_invalid_conf(void) {
	assert(opt_abort_conf);
	malloc_printf("<jemalloc>: Abort (abort_conf:true) on invalid conf "
	    "value (see above).\n");
	abort();
}

static void
malloc_conf_error(const char *msg, const char *k, size_t klen, const char *v,
    size_t vlen) {
	malloc_printf("<jemalloc>: %s: %.*s:%.*s\n", msg, (int)klen, k,
	    (int)vlen, v);
	/* If abort_conf is set, error out after processing all options. */
	const char *experimental = "experimental_";
	if (strncmp(k, experimental, strlen(experimental)) == 0) {
		/* However, tolerate experimental features. */
		return;
	}
	had_conf_error = true;
}

static const char *
obtain_malloc_conf(unsigned which_source, char buf[PATH_MAX + 1]) {
	if (config_debug) {
		static unsigned read_source = 0;
		/*
		 * Each source should only be read once, to minimize # of
		 * syscalls on init.
		 */
		assert(read_source++ == which_source);
	}
	assert(which_source < MALLOC_CONF_NSOURCES);

	const char *ret;
	switch (which_source) {
	case 0:
		ret = config_malloc_conf;
		break;
	case 1:
		if (je_malloc_conf != NULL) {
			/* Use options that were compiled into the program. */
			ret = je_malloc_conf;
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	case 2: {
		ssize_t linklen = 0;
#ifndef _WIN32
		int saved_errno = errno;
		const char *linkname =
#  ifdef JEMALLOC_PREFIX
		    "/etc/"JEMALLOC_PREFIX"malloc.conf"
#  else
		    "/etc/malloc.conf"
#  endif
		    ;

		/*
		 * Try to use the contents of the "/etc/malloc.conf" symbolic
		 * link's name.
		 */
#ifndef JEMALLOC_READLINKAT
		linklen = readlink(linkname, buf, PATH_MAX);
#else
		linklen = readlinkat(AT_FDCWD, linkname, buf, PATH_MAX);
#endif
		if (linklen == -1) {
			/* No configuration specified. */
			linklen = 0;
			/* Restore errno. */
			set_errno(saved_errno);
		}
#endif
		buf[linklen] = '\0';
		ret = buf;
		break;
	} case 3: {
		const char *envname =
#ifdef JEMALLOC_PREFIX
		    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
		    "MALLOC_CONF"
#endif
		    ;

		if ((ret = jemalloc_secure_getenv(envname)) != NULL) {
			/*
			 * Do nothing; opts is already initialized to the value
			 * of the MALLOC_CONF environment variable.
			 */
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	} case 4: {
		ret = je_malloc_conf_2_conf_harder;
		break;
	} default:
		not_reached();
		ret = NULL;
	}
	return ret;
}

static void
malloc_conf_init_helper(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS],
    bool initial_call, const char *opts_cache[MALLOC_CONF_NSOURCES],
    char buf[PATH_MAX + 1]) {
	static const char *opts_explain[MALLOC_CONF_NSOURCES] = {
		"string specified via --with-malloc-conf",
		"string pointed to by the global variable malloc_conf",
		"\"name\" of the file referenced by the symbolic link named "
		    "/etc/malloc.conf",
		"value of the environment variable MALLOC_CONF",
		"string pointed to by the global variable "
		    "malloc_conf_2_conf_harder",
	};
	unsigned i;
	const char *opts, *k, *v;
	size_t klen, vlen;

	for (i = 0; i < MALLOC_CONF_NSOURCES; i++) {
		/* Get runtime configuration. */
		if (initial_call) {
			opts_cache[i] = obtain_malloc_conf(i, buf);
		}
		opts = opts_cache[i];
		if (!initial_call && opt_confirm_conf) {
			malloc_printf(
			    "<jemalloc>: malloc_conf #%u (%s): \"%s\"\n",
			    i + 1, opts_explain[i], opts != NULL ? opts : "");
		}
		if (opts == NULL) {
			continue;
		}

		while (*opts != '\0' && !malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen)) {

#define CONF_ERROR(msg, k, klen, v, vlen)				\
			if (!initial_call) {				\
				malloc_conf_error(			\
				    msg, k, klen, v, vlen);		\
				cur_opt_valid = false;			\
			}
#define CONF_CONTINUE	{						\
				if (!initial_call && opt_confirm_conf	\
				    && cur_opt_valid) {			\
					malloc_printf("<jemalloc>: -- "	\
					    "Set conf value: %.*s:%.*s"	\
					    "\n", (int)klen, k,		\
					    (int)vlen, v);		\
				}					\
				continue;				\
			}
#define CONF_MATCH(n)							\
	(sizeof(n)-1 == klen && strncmp(n, k, klen) == 0)
#define CONF_MATCH_VALUE(n)						\
	(sizeof(n)-1 == vlen && strncmp(n, v, vlen) == 0)
#define CONF_HANDLE_BOOL(o, n)						\
			if (CONF_MATCH(n)) {				\
				if (CONF_MATCH_VALUE("true")) {		\
					o = true;			\
				} else if (CONF_MATCH_VALUE("false")) {	\
					o = false;			\
				} else {				\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				}					\
				CONF_CONTINUE;				\
			}
      /*
       * One of the CONF_MIN macros below expands, in one of the use points,
       * to "unsigned integer < 0", which is always false, triggering the
       * GCC -Wtype-limits warning, which we disable here and re-enable below.
       */
      JEMALLOC_DIAGNOSTIC_PUSH
      JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS

#define CONF_DONT_CHECK_MIN(um, min)	false
#define CONF_CHECK_MIN(um, min)	((um) < (min))
#define CONF_DONT_CHECK_MAX(um, max)	false
#define CONF_CHECK_MAX(um, max)	((um) > (max))

#define CONF_HANDLE_T(t, max_t, o, n, min, max, check_min, check_max, clip) \
			if (CONF_MATCH(n)) {				\
				max_t mv;				\
				char *end;				\
									\
				set_errno(0);				\
				mv = (max_t)malloc_strtoumax(v, &end, 0); \
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (clip) {			\
					if (check_min(mv, (t)(min))) {	\
						o = (t)(min);		\
					} else if (			\
					    check_max(mv, (t)(max))) {	\
						o = (t)(max);		\
					} else {			\
						o = (t)mv;		\
					}				\
				} else {				\
					if (check_min(mv, (t)(min)) ||	\
					    check_max(mv, (t)(max))) {	\
						CONF_ERROR(		\
						    "Out-of-range "	\
						    "conf value",	\
						    k, klen, v, vlen);	\
					} else {			\
						o = (t)mv;		\
					}				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_T_U(t, o, n, min, max, check_min, check_max, clip)	\
	      CONF_HANDLE_T(t, uintmax_t, o, n, min, max, check_min,	\
			    check_max, clip)
#define CONF_HANDLE_T_SIGNED(t, o, n, min, max, check_min, check_max, clip)\
	      CONF_HANDLE_T(t, intmax_t, o, n, min, max, check_min,	\
			    check_max, clip)

#define CONF_HANDLE_UNSIGNED(o, n, min, max, check_min, check_max,	\
    clip)								\
			CONF_HANDLE_T_U(unsigned, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SIZE_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_U(size_t, o, n, min, max,		\
			    check_min, check_max, clip)
#define CONF_HANDLE_INT64_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_SIGNED(int64_t, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			CONF_HANDLE_T_SIGNED(ssize_t, o, n, min, max,	\
			    CONF_CHECK_MIN, CONF_CHECK_MAX, false)
#define CONF_HANDLE_CHAR_P(o, n, d)					\
			if (CONF_MATCH(n)) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(o)-1) ? vlen :		\
				    sizeof(o)-1;			\
				strncpy(o, v, cpylen);			\
				o[cpylen] = '\0';			\
				CONF_CONTINUE;				\
			}

			bool cur_opt_valid = true;

			CONF_HANDLE_BOOL(opt_confirm_conf, "confirm_conf")
			if (initial_call) {
				continue;
			}

			CONF_HANDLE_BOOL(opt_abort, "abort")
			CONF_HANDLE_BOOL(opt_abort_conf, "abort_conf")
			if (strncmp("metadata_thp", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < metadata_thp_mode_limit; i++) {
					if (strncmp(metadata_thp_mode_names[i],
					    v, vlen) == 0) {
						opt_metadata_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_retain, "retain")
			if (strncmp("dss", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < dss_prec_limit; i++) {
					if (strncmp(dss_prec_names[i], v, vlen)
					    == 0) {
						if (extent_dss_prec_set(i)) {
							CONF_ERROR(
							    "Error setting dss",
							    k, klen, v, vlen);
						} else {
							opt_dss =
							    dss_prec_names[i];
							match = true;
							break;
						}
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			if (CONF_MATCH("narenas")) {
				if (CONF_MATCH_VALUE("default")) {
					opt_narenas = 0;
					CONF_CONTINUE;
				} else {
					CONF_HANDLE_UNSIGNED(opt_narenas,
					    "narenas", 1, UINT_MAX,
					    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
					    /* clip */ false)
				}
			}
			if (CONF_MATCH("narenas_ratio")) {
				CONF_HANDLE_UNSIGNED(opt_narenas_ratio,
				    "narenas_ratio", 1, UINT_MAX,
				    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
				    /* clip */ false)
			}
			if (CONF_MATCH("bin_shards")) {
				const char *bin_shards_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t size_start;
					size_t size_end;
					size_t nshards;
					bool err = malloc_conf_multi_sizes_next(
					    &bin_shards_segment_cur, &vlen_left,
					    &size_start, &size_end, &nshards);
					if (err || bin_update_shard_size(
					    bin_shard_sizes, size_start,
					    size_end, nshards)) {
						CONF_ERROR(
						    "Invalid settings for "
						    "bin_shards", k, klen, v,
						    vlen);
						break;
					}
				} while (vlen_left > 0);
				CONF_CONTINUE;
			}
			CONF_HANDLE_SSIZE_T(opt_dirty_decay_ms,
			    "dirty_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_SSIZE_T(opt_muzzy_decay_ms,
			    "muzzy_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_BOOL(opt_stats_print, "stats_print")
			if (CONF_MATCH("stats_print_opts")) {
				init_opt_stats_opts(v, vlen,
				    opt_stats_print_opts);
				CONF_CONTINUE;
			}
			CONF_HANDLE_INT64_T(opt_stats_interval,
			    "stats_interval", -1, INT64_MAX,
			    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX, false)
			if (CONF_MATCH("stats_interval_opts")) {
				init_opt_stats_opts(v, vlen,
				    opt_stats_interval_opts);
				CONF_CONTINUE;
			}
			if (config_fill) {
				if (CONF_MATCH("junk")) {
					if (CONF_MATCH_VALUE("true")) {
						opt_junk = "true";
						opt_junk_alloc = opt_junk_free =
						    true;
					} else if (CONF_MATCH_VALUE("false")) {
						opt_junk = "false";
						opt_junk_alloc = opt_junk_free =
						    false;
					} else if (CONF_MATCH_VALUE("alloc")) {
						opt_junk = "alloc";
						opt_junk_alloc = true;
						opt_junk_free = false;
					} else if (CONF_MATCH_VALUE("free")) {
						opt_junk = "free";
						opt_junk_alloc = false;
						opt_junk_free = true;
					} else {
						CONF_ERROR(
						    "Invalid conf value",
						    k, klen, v, vlen);
					}
					CONF_CONTINUE;
				}
				CONF_HANDLE_BOOL(opt_zero, "zero")
			}
			if (config_utrace) {
				CONF_HANDLE_BOOL(opt_utrace, "utrace")
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(opt_xmalloc, "xmalloc")
			}
			CONF_HANDLE_BOOL(opt_tcache, "tcache")
			CONF_HANDLE_SSIZE_T(opt_lg_tcache_max, "lg_tcache_max",
			    -1, (sizeof(size_t) << 3) - 1)
			/*
			 * Anyone trying to set a value outside -16 to 16 is
			 * deeply confused.
			 */
			CONF_HANDLE_SSIZE_T(opt_lg_tcache_nslots_mul,
			    "lg_tcache_nslots_mul", -16, 16)
			/* Ditto with values past 2048. */
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_small_min,
			    "tcache_nslots_small_min", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_small_max,
			    "tcache_nslots_small_max", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_large,
			    "tcache_nslots_large", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_SIZE_T(opt_tcache_gc_incr_bytes,
			    "tcache_gc_incr_bytes", 1024, SIZE_T_MAX,
			    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    /* clip */ true)
			CONF_HANDLE_SIZE_T(opt_tcache_gc_delay_bytes,
			    "tcache_gc_delay_bytes", 0, SIZE_T_MAX,
			    CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    /* clip */ false)
			CONF_HANDLE_UNSIGNED(opt_lg_tcache_flush_small_div,
			    "lg_tcache_flush_small_div", 1, 16,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_lg_tcache_flush_large_div,
			    "lg_tcache_flush_large_div", 1, 16,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)

			/*
			 * The runtime option of oversize_threshold remains
			 * undocumented.  It may be tweaked in the next major
			 * release (6.0).  The default value 8M is rather
			 * conservative / safe.  Tuning it further down may
			 * improve fragmentation a bit more, but may also cause
			 * contention on the huge arena.
			 */
			CONF_HANDLE_SIZE_T(opt_oversize_threshold,
			    "oversize_threshold", 0, SC_LARGE_MAXCLASS,
			    CONF_DONT_CHECK_MIN, CONF_CHECK_MAX, false)
			CONF_HANDLE_SIZE_T(opt_lg_extent_max_active_fit,
			    "lg_extent_max_active_fit", 0,
			    (sizeof(size_t) << 3), CONF_DONT_CHECK_MIN,
			    CONF_CHECK_MAX, false)

			if (strncmp("percpu_arena", k, klen) == 0) {
				bool match = false;
				for (int i = percpu_arena_mode_names_base; i <
				    percpu_arena_mode_names_limit; i++) {
					if (strncmp(percpu_arena_mode_names[i],
					    v, vlen) == 0) {
						if (!have_percpu_arena) {
							CONF_ERROR(
							    "No getcpu support",
							    k, klen, v, vlen);
						}
						opt_percpu_arena = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_background_thread,
			    "background_thread");
			CONF_HANDLE_SIZE_T(opt_max_background_threads,
					   "max_background_threads", 1,
					   opt_max_background_threads,
					   CONF_CHECK_MIN, CONF_CHECK_MAX,
					   true);
			if (CONF_MATCH("slab_sizes")) {
				bool err;
				const char *slab_size_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t slab_start;
					size_t slab_end;
					size_t pgs;
					err = malloc_conf_multi_sizes_next(
					    &slab_size_segment_cur,
					    &vlen_left, &slab_start, &slab_end,
					    &pgs);
					if (!err) {
						sc_data_update_slab_size(
						    sc_data, slab_start,
						    slab_end, (int)pgs);
					} else {
						CONF_ERROR("Invalid settings "
						    "for slab_sizes",
						    k, klen, v, vlen);
					}
				} while (!err && vlen_left > 0);
				CONF_CONTINUE;
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(opt_prof, "prof")
				CONF_HANDLE_CHAR_P(opt_prof_prefix,
				    "prof_prefix", "jeprof")
				CONF_HANDLE_BOOL(opt_prof_active, "prof_active")
				CONF_HANDLE_BOOL(opt_prof_thread_active_init,
				    "prof_thread_active_init")
				CONF_HANDLE_SIZE_T(opt_lg_prof_sample,
				    "lg_prof_sample", 0, (sizeof(uint64_t) << 3)
				    - 1, CONF_DONT_CHECK_MIN, CONF_CHECK_MAX,
				    true)
				CONF_HANDLE_BOOL(opt_prof_accum, "prof_accum")
				CONF_HANDLE_SSIZE_T(opt_lg_prof_interval,
				    "lg_prof_interval", -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_gdump, "prof_gdump")
				CONF_HANDLE_BOOL(opt_prof_final, "prof_final")
				CONF_HANDLE_BOOL(opt_prof_leak, "prof_leak")
				CONF_HANDLE_BOOL(opt_prof_log, "prof_log")
				CONF_HANDLE_SSIZE_T(opt_prof_recent_alloc_max,
				    "prof_recent_alloc_max", -1, SSIZE_MAX)
				CONF_HANDLE_BOOL(opt_prof_sys_thread_name,
				    "prof_sys_thread_name")
				if (CONF_MATCH("prof_time_resolution")) {
					if (CONF_MATCH_VALUE("default")) {
						opt_prof_time_res =
						    prof_time_res_default;
					} else if (CONF_MATCH_VALUE("high")) {
						if (!config_high_res_timer) {
							CONF_ERROR(
							    "No high resolution"
							    " timer support",
							    k, klen, v, vlen);
						} else {
							opt_prof_time_res =
							    prof_time_res_high;
						}
					} else {
						CONF_ERROR("Invalid conf value",
						    k, klen, v, vlen);
					}
					CONF_CONTINUE;
				}
				/*
				 * Undocumented.  When set to false, don't
				 * correct for an unbiasing bug in jeprof
				 * attribution.  This can be handy if you want
				 * to get consistent numbers from your binary
				 * across different jemalloc versions, even if
				 * those numbers are incorrect.  The default is
				 * true.
				 */
				CONF_HANDLE_BOOL(opt_prof_unbias, "prof_unbias")
			}
			if (config_log) {
				if (CONF_MATCH("log")) {
					size_t cpylen = (
					    vlen <= sizeof(log_var_names) ?
					    vlen : sizeof(log_var_names) - 1);
					strncpy(log_var_names, v, cpylen);
					log_var_names[cpylen] = '\0';
					CONF_CONTINUE;
				}
			}
			if (CONF_MATCH("thp")) {
				bool match = false;
				for (int i = 0; i < thp_mode_names_limit; i++) {
					if (strncmp(thp_mode_names[i],v, vlen)
					    == 0) {
						if (!have_madvise_huge
						    && !have_memcntl) {
							CONF_ERROR(
							    "No THP support",
							    k, klen, v, vlen);
						}
						opt_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			if (CONF_MATCH("zero_realloc")) {
				if (CONF_MATCH_VALUE("strict")) {
					opt_zero_realloc_action
					    = zero_realloc_action_strict;
				} else if (CONF_MATCH_VALUE("free")) {
					opt_zero_realloc_action
					    = zero_realloc_action_free;
				} else if (CONF_MATCH_VALUE("abort")) {
					opt_zero_realloc_action
					    = zero_realloc_action_abort;
				} else {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_ERROR("Invalid conf pair", k, klen, v, vlen);
#undef CONF_ERROR
#undef CONF_CONTINUE
#undef CONF_MATCH
#undef CONF_MATCH_VALUE
#undef CONF_HANDLE_BOOL
#undef CONF_DONT_CHECK_MIN
#undef CONF_CHECK_MIN
#undef CONF_DONT_CHECK_MAX
#undef CONF_CHECK_MAX
#undef CONF_HANDLE_T
#undef CONF_HANDLE_T_U
#undef CONF_HANDLE_T_SIGNED
#undef CONF_HANDLE_UNSIGNED
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
    /* Re-enable diagnostic "-Wtype-limits" */
    JEMALLOC_DIAGNOSTIC_POP
		}
		if (opt_abort_conf && had_conf_error) {
			malloc_abort_invalid_conf();
		}
	}
	atomic_store_b(&log_init_done, true, ATOMIC_RELEASE);
}

void
malloc_conf_init(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]) {
	const char *opts_cache[MALLOC_CONF_NSOURCES] = {NULL, NULL, NULL, NULL,
		NULL};
	char buf[PATH_MAX + 1];

	/* The first call only set the confirm_conf option and opts_cache */
	malloc_conf_init_helper(NULL, NULL, true, opts_cache, buf);
	malloc_conf_init_helper(sc_data, bin_shard_sizes, false, opts_cache,
	    NULL);
}
