#ifndef JEMALLOC_INTERNAL_OS_PROC_MAPS_H
#define JEMALLOC_INTERNAL_OS_PROC_MAPS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/os/detect.h"

/*
 * Process-maps interface: pid-namespace lookup and MAPPED_LIBRARIES dumping
 * for profiling (used in src/prof_sys.c).
 * Default: posix/ (Linux-style /proc). Override: FreeBSD/DragonFly
 * (/proc/curproc/... paths), Darwin (no /proc; walks dyld images instead),
 * Windows (neither concept is implemented).
 */

/*
 * Parse a procfs pid-namespace symlink target of the form "pid:[<id>]".
 * This helper function is currently used only by posix and FreeBSD/DragonFly
 * backends.
 */
JEMALLOC_ALWAYS_INLINE uint64_t
os_prof_parse_pid_namespace(const char *buf, size_t buf_size, ssize_t linklen) {
	static const char prefix[] = "pid:[";
	const size_t      prefix_len = sizeof(prefix) - 1;

	if (linklen <= 0 || (size_t)linklen >= buf_size) {
		return 0;
	}
	size_t len = (size_t)linklen;
	if (len <= prefix_len + 1 || buf[len - 1] != ']'
	    || memcmp(buf, prefix, prefix_len) != 0) {
		return 0;
	}

	uint64_t ns = 0;
	for (size_t i = prefix_len; i < len - 1; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c < '0' || c > '9') {
			return 0;
		}
		uint64_t digit = (uint64_t)(c - '0');
		if (ns > (UINT64_MAX - digit) / 10) {
			return 0;
		}
		ns = ns * 10 + digit;
	}
	return ns;
}

/* Functions required for implementation in each backend. */
JEMALLOC_ALWAYS_INLINE uint64_t os_prof_pid_namespace(void);
JEMALLOC_ALWAYS_INLINE int os_prof_open_maps(void);
/*
 * Takes the caller's current prof_dump_open_maps hook as a parameter instead
 * of reaching for prof_sys.h's JET_MUTABLE global directly: prof_sys.h pulls
 * in prof.h -> mutex.h -> os.h, which would re-enter this file mid-expansion.
 * Darwin ignores the parameter (no fd-based maps to open there).
 */
JEMALLOC_ALWAYS_INLINE void os_prof_dump_maps(
    buf_writer_t *buf_writer, int (*open_maps)(void));

#if defined(_WIN32)
#  include "jemalloc/internal/os/windows/proc_maps.h"
#elif defined(__APPLE__)
#  include "jemalloc/internal/os/darwin/proc_maps.h"
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#  include "jemalloc/internal/os/freebsd/proc_maps.h"
#elif defined(JEMALLOC_OS_POSIX)
#  include "jemalloc/internal/os/posix/proc_maps.h"
#else
#  error "OS layer: no proc_maps backend for this platform; add os/<os>/proc_maps.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_PROC_MAPS_H */
