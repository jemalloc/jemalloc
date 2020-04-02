#define JEMALLOC_PROF_SYS_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_sys.h"

malloc_mutex_t prof_dump_filename_mtx;

static uint64_t prof_dump_seq;
static uint64_t prof_dump_iseq;
static uint64_t prof_dump_mseq;
static uint64_t prof_dump_useq;

static char *prof_dump_prefix = NULL;

/* The fallback allocator profiling functionality will use. */
base_t *prof_base;

/* The following are needed for dumping and are protected by prof_dump_mtx. */
/*
 * Whether there has been an error in the dumping process, which could have
 * happened either in file opening or in file writing.  When an error has
 * already occurred, we will stop further writing to the file.
 */
static bool prof_dump_error;
/*
 * Whether error should be handled locally: if true, then we print out error
 * message as well as abort (if opt_abort is true) when an error occurred, and
 * we also report the error back to the caller in the end; if false, then we
 * only report the error back to the caller in the end.
 */
static bool prof_dump_handle_error_locally;
/*
 * This buffer is rather large for stack allocation, so use a single buffer for
 * all profile dumps.
 */
static char prof_dump_buf[PROF_DUMP_BUFSIZE];
static size_t prof_dump_buf_end;
static int prof_dump_fd;

static int
prof_sys_thread_name_read_impl(char *buf, size_t limit) {
#ifdef JEMALLOC_HAVE_PTHREAD_SETNAME_NP
	return pthread_getname_np(pthread_self(), buf, limit);
#else
	return ENOSYS;
#endif
}
prof_sys_thread_name_read_t *JET_MUTABLE prof_sys_thread_name_read =
    prof_sys_thread_name_read_impl;

void
prof_sys_thread_name_fetch(tsd_t *tsd) {
#define THREAD_NAME_MAX_LEN 16
	char buf[THREAD_NAME_MAX_LEN];
	if (!prof_sys_thread_name_read(buf, THREAD_NAME_MAX_LEN)) {
		prof_thread_name_set_impl(tsd, buf);
	}
#undef THREAD_NAME_MAX_LEN
}

static void
prof_dump_check_possible_error(bool err_cond, const char *format, ...) {
	assert(!prof_dump_error);
	if (!err_cond) {
		return;
	}

	prof_dump_error = true;
	if (!prof_dump_handle_error_locally) {
		return;
	}

	va_list ap;
	char buf[PROF_PRINTF_BUFSIZE];
	va_start(ap, format);
	malloc_vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	malloc_write(buf);

	if (opt_abort) {
		abort();
	}
}

static int
prof_dump_open_file_impl(const char *filename, int mode) {
	return creat(filename, mode);
}
prof_dump_open_file_t *JET_MUTABLE prof_dump_open_file =
    prof_dump_open_file_impl;

static void
prof_dump_open(const char *filename) {
	prof_dump_fd = prof_dump_open_file(filename, 0644);
	prof_dump_check_possible_error(prof_dump_fd == -1,
	    "<jemalloc>: failed to open \"%s\"\n", filename);
}

prof_dump_write_file_t *JET_MUTABLE prof_dump_write_file = malloc_write_fd;

static void
prof_dump_flush() {
	cassert(config_prof);
	if (!prof_dump_error) {
		ssize_t err = prof_dump_write_file(prof_dump_fd, prof_dump_buf,
		    prof_dump_buf_end);
		prof_dump_check_possible_error(err == -1,
		    "<jemalloc>: failed to write during heap profile flush\n");
	}
	prof_dump_buf_end = 0;
}

static void
prof_dump_write(const char *s) {
	size_t i, slen, n;

	cassert(config_prof);

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the buffer if it is full. */
		if (prof_dump_buf_end == PROF_DUMP_BUFSIZE) {
			prof_dump_flush();
		}

		if (prof_dump_buf_end + slen - i <= PROF_DUMP_BUFSIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = PROF_DUMP_BUFSIZE - prof_dump_buf_end;
		}
		memcpy(&prof_dump_buf[prof_dump_buf_end], &s[i], n);
		prof_dump_buf_end += n;
		i += n;
	}
	assert(i == slen);
}

static void
prof_dump_close() {
	if (prof_dump_fd != -1) {
		prof_dump_flush();
		close(prof_dump_fd);
	}
}

#ifndef _WIN32
JEMALLOC_FORMAT_PRINTF(1, 2)
static int
prof_open_maps_internal(const char *format, ...) {
	int mfd;
	va_list ap;
	char filename[PATH_MAX + 1];

	va_start(ap, format);
	malloc_vsnprintf(filename, sizeof(filename), format, ap);
	va_end(ap);

#if defined(O_CLOEXEC)
	mfd = open(filename, O_RDONLY | O_CLOEXEC);
#else
	mfd = open(filename, O_RDONLY);
	if (mfd != -1) {
		fcntl(mfd, F_SETFD, fcntl(mfd, F_GETFD) | FD_CLOEXEC);
	}
#endif

	return mfd;
}
#endif

static int
prof_dump_open_maps_impl() {
	int mfd;

	cassert(config_prof);
#ifdef __FreeBSD__
	mfd = prof_open_maps_internal("/proc/curproc/map");
#elif defined(_WIN32)
	mfd = -1; // Not implemented
#else
	int pid = prof_getpid();

	mfd = prof_open_maps_internal("/proc/%d/task/%d/maps", pid, pid);
	if (mfd == -1) {
		mfd = prof_open_maps_internal("/proc/%d/maps", pid);
	}
#endif
	return mfd;
}
prof_dump_open_maps_t *JET_MUTABLE prof_dump_open_maps =
    prof_dump_open_maps_impl;

static void
prof_dump_maps() {
	int mfd = prof_dump_open_maps();
	if (mfd == -1) {
		return;
	}

	prof_dump_write("\nMAPPED_LIBRARIES:\n");
	ssize_t nread = 0;
	do {
		prof_dump_buf_end += nread;
		if (prof_dump_buf_end == PROF_DUMP_BUFSIZE) {
			/* Make space in prof_dump_buf before read(). */
			prof_dump_flush();
		}
		nread = malloc_read_fd(mfd, &prof_dump_buf[prof_dump_buf_end],
		    PROF_DUMP_BUFSIZE - prof_dump_buf_end);
	} while (nread > 0);

	close(mfd);
}

static bool
prof_dump(tsd_t *tsd, bool propagate_err, const char *filename,
    bool leakcheck) {
	cassert(config_prof);
	assert(tsd_reentrancy_level_get(tsd) == 0);

	prof_tdata_t * tdata = prof_tdata_get(tsd, true);
	if (tdata == NULL) {
		return true;
	}

	prof_dump_error = false;
	prof_dump_handle_error_locally = !propagate_err;

	pre_reentrancy(tsd, NULL);
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_mtx);

	prof_dump_open(filename);
	prof_dump_impl(tsd, tdata, prof_dump_write, leakcheck);
	prof_dump_maps();
	prof_dump_close();

	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_mtx);
	post_reentrancy(tsd);

	return prof_dump_error;
}

/*
 * If profiling is off, then PROF_DUMP_FILENAME_LEN is 1, so we'll end up
 * calling strncpy with a size of 0, which triggers a -Wstringop-truncation
 * warning (strncpy can never actually be called in this case, since we bail out
 * much earlier when config_prof is false).  This function works around the
 * warning to let us leave the warning on.
 */
static inline void
prof_strncpy(char *UNUSED dest, const char *UNUSED src, size_t UNUSED size) {
	cassert(config_prof);
#ifdef JEMALLOC_PROF
	strncpy(dest, src, size);
#endif
}

static const char *
prof_dump_prefix_get(tsdn_t* tsdn) {
	malloc_mutex_assert_owner(tsdn, &prof_dump_filename_mtx);

	return prof_dump_prefix == NULL ? opt_prof_prefix : prof_dump_prefix;
}

static bool
prof_dump_prefix_is_empty(tsdn_t *tsdn) {
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	bool ret = (prof_dump_prefix_get(tsdn)[0] == '\0');
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
	return ret;
}

#define DUMP_FILENAME_BUFSIZE (PATH_MAX + 1)
#define VSEQ_INVALID UINT64_C(0xffffffffffffffff)
static void
prof_dump_filename(tsd_t *tsd, char *filename, char v, uint64_t vseq) {
	cassert(config_prof);

	assert(tsd_reentrancy_level_get(tsd) == 0);
	const char *prof_prefix = prof_dump_prefix_get(tsd_tsdn(tsd));

	if (vseq != VSEQ_INVALID) {
	        /* "<prefix>.<pid>.<seq>.v<vseq>.heap" */
		malloc_snprintf(filename, DUMP_FILENAME_BUFSIZE,
		    "%s.%d.%"FMTu64".%c%"FMTu64".heap",
		    prof_prefix, prof_getpid(), prof_dump_seq, v, vseq);
	} else {
	        /* "<prefix>.<pid>.<seq>.<v>.heap" */
		malloc_snprintf(filename, DUMP_FILENAME_BUFSIZE,
		    "%s.%d.%"FMTu64".%c.heap",
		    prof_prefix, prof_getpid(), prof_dump_seq, v);
	}
	prof_dump_seq++;
}

void
prof_get_default_filename(tsdn_t *tsdn, char *filename, uint64_t ind) {
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	malloc_snprintf(filename, PROF_DUMP_FILENAME_LEN,
	    "%s.%d.%"FMTu64".json", prof_dump_prefix_get(tsdn), prof_getpid(),
	    ind);
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
}

void
prof_fdump_impl(tsd_t *tsd) {
	char filename[DUMP_FILENAME_BUFSIZE];

	assert(!prof_dump_prefix_is_empty(tsd_tsdn(tsd)));
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump_filename(tsd, filename, 'f', VSEQ_INVALID);
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, opt_prof_leak);
}

bool
prof_dump_prefix_set(tsdn_t *tsdn, const char *prefix) {
	cassert(config_prof);
	ctl_mtx_assert_held(tsdn);
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	if (prof_dump_prefix == NULL) {
		malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
		/* Everything is still guarded by ctl_mtx. */
		char *buffer = base_alloc(tsdn, prof_base,
		    PROF_DUMP_FILENAME_LEN, QUANTUM);
		if (buffer == NULL) {
			return true;
		}
		malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
		prof_dump_prefix = buffer;
	}
	assert(prof_dump_prefix != NULL);

	prof_strncpy(prof_dump_prefix, prefix, PROF_DUMP_FILENAME_LEN - 1);
	prof_dump_prefix[PROF_DUMP_FILENAME_LEN - 1] = '\0';
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);

	return false;
}

void
prof_idump_impl(tsd_t *tsd) {
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	if (prof_dump_prefix_get(tsd_tsdn(tsd))[0] == '\0') {
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		return;
	}
	char filename[PATH_MAX + 1];
	prof_dump_filename(tsd, filename, 'i', prof_dump_iseq);
	prof_dump_iseq++;
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, false);
}

bool
prof_mdump_impl(tsd_t *tsd, const char *filename) {
	char filename_buf[DUMP_FILENAME_BUFSIZE];
	if (filename == NULL) {
		/* No filename specified, so automatically generate one. */
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		if (prof_dump_prefix_get(tsd_tsdn(tsd))[0] == '\0') {
			malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
			return true;
		}
		prof_dump_filename(tsd, filename_buf, 'm', prof_dump_mseq);
		prof_dump_mseq++;
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_dump_filename_mtx);
		filename = filename_buf;
	}
	return prof_dump(tsd, true, filename, false);
}

void
prof_gdump_impl(tsd_t *tsd) {
	tsdn_t *tsdn = tsd_tsdn(tsd);
	malloc_mutex_lock(tsdn, &prof_dump_filename_mtx);
	if (prof_dump_prefix_get(tsdn)[0] == '\0') {
		malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
		return;
	}
	char filename[DUMP_FILENAME_BUFSIZE];
	prof_dump_filename(tsd, filename, 'u', prof_dump_useq);
	prof_dump_useq++;
	malloc_mutex_unlock(tsdn, &prof_dump_filename_mtx);
	prof_dump(tsd, false, filename, false);
}
