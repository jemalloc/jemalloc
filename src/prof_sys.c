#define JEMALLOC_PROF_SYS_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_sys.h"

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
