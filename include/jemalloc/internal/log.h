#ifndef JEMALLOC_INTERNAL_LOG_H
#define JEMALLOC_INTERNAL_LOG_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"

#ifdef JEMALLOC_LOG
#  define JEMALLOC_LOG_VAR_BUFSIZE 1000
#else
#  define JEMALLOC_LOG_VAR_BUFSIZE 1
#endif

#define JEMALLOC_LOG_BUFSIZE 4096

/*
 * The log_vars malloc_conf option is a '|'-delimited list of log_var name
 * segments to log.  The log_var names are themselves hierarchical, with '.' as
 * the delimiter (a "segment" is just a prefix in the log namespace).  So, if
 * you have:
 *
 * static log_var_t log_arena = LOG_VAR_INIT("arena"); // 1
 * static log_var_t log_arena_a = LOG_VAR_INIT("arena.a"); // 2
 * static log_var_t log_arena_b = LOG_VAR_INIT("arena.b"); // 3
 * static log_var_t log_arena_a_a = LOG_VAR_INIT("arena.a.a"); // 4
 * static_log_var_t log_extent_a = LOG_VAR_INIT("extent.a"); // 5
 * static_log_var_t log_extent_b = LOG_VAR_INIT("extent.b"); // 6
 *
 * And your malloc_conf option is "log_vars=arena.a|extent", then log_vars 2, 4,
 * 5, and 6 will be enabled.  You can enable logging from all log vars by
 * writing "log_vars=.".
 *
 * You can then log by writing:
 *   log(log_var, "format string -- my int is %d", my_int);
 *
 * The namespaces currently in use:
 *   core.[malloc|free|posix_memalign|...].[entry|exit]:
 *       The entry/exit points of the functions publicly exposed by jemalloc.
 *       The "entry" variants try to log arguments to the functions, and the
 *       "exit" ones try to log return values.
 *
 * None of this should be regarded as a stable API for right now.  It's intended
 * as a debugging interface, to let us keep around some of our printf-debugging
 * statements.
 */

extern char log_var_names[JEMALLOC_LOG_VAR_BUFSIZE];
extern atomic_b_t log_init_done;

typedef struct log_var_s log_var_t;
struct log_var_s {
	/*
	 * Lowest bit is "inited", second lowest is "enabled".  Putting them in
	 * a single word lets us avoid any fences on weak architectures.
	 */
	atomic_u_t state;
	const char *name;
};

#define LOG_NOT_INITIALIZED 0U
#define LOG_INITIALIZED_NOT_ENABLED 1U
#define LOG_ENABLED 2U

#define LOG_VAR_INIT(name_str) {ATOMIC_INIT(LOG_NOT_INITIALIZED), name_str}

/*
 * Returns the value we should assume for state (which is not necessarily
 * accurate; if logging is done before logging has finished initializing, then
 * we default to doing the safe thing by logging everything).
 */
unsigned log_var_update_state(log_var_t *log_var);

/* We factor out the metadata management to allow us to test more easily. */
#define log_do_begin(log_var)						\
if (config_log) {							\
	unsigned log_state = atomic_load_u(&(log_var).state,		\
	    ATOMIC_RELAXED);						\
	if (unlikely(log_state == LOG_NOT_INITIALIZED)) {		\
		log_state = log_var_update_state(&(log_var));		\
		assert(log_state != LOG_NOT_INITIALIZED);		\
	}								\
	if (log_state == LOG_ENABLED) {					\
		{
			/* User code executes here. */
#define log_do_end(log_var)						\
		}							\
	}								\
}

/*
 * MSVC has some preprocessor bugs in its expansion of __VA_ARGS__ during
 * preprocessing.  To work around this, we take all potential extra arguments in
 * a var-args functions.  Since a varargs macro needs at least one argument in
 * the "...", we accept the format string there, and require that the first
 * argument in this "..." is a const char *.
 */
static inline void
log_impl_varargs(const char *name, ...) {
	char buf[JEMALLOC_LOG_BUFSIZE];
	va_list ap;

	va_start(ap, name);
	const char *format = va_arg(ap, const char *);
	size_t dst_offset = 0;
	dst_offset += malloc_snprintf(buf, JEMALLOC_LOG_BUFSIZE, "%s: ", name);
	dst_offset += malloc_vsnprintf(buf + dst_offset,
	    JEMALLOC_LOG_BUFSIZE - dst_offset, format, ap);
	dst_offset += malloc_snprintf(buf + dst_offset,
	    JEMALLOC_LOG_BUFSIZE - dst_offset, "\n");
	va_end(ap);

	malloc_write(buf);
}

/* Call as log("log.var.str", "format_string %d", arg_for_format_string); */
#define log(log_var_str, ...)						\
do {									\
	static log_var_t log_var = LOG_VAR_INIT(log_var_str);		\
	log_do_begin(log_var)						\
		log_impl_varargs((log_var).name, __VA_ARGS__);		\
	log_do_end(log_var)						\
} while (0)

#endif /* JEMALLOC_INTERNAL_LOG_H */
