#ifndef JEMALLOC_INTERNAL_OS_FREEBSD_PROC_MAPS_H
#define JEMALLOC_INTERNAL_OS_FREEBSD_PROC_MAPS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

JEMALLOC_ALWAYS_INLINE long
os_prof_pid_namespace(void) {
	long ret = 0;
	char buf[PATH_MAX];
	ssize_t linklen =
#ifndef JEMALLOC_READLINKAT
	    readlink("/proc/curproc/ns/pid", buf, PATH_MAX)
#else
	    readlinkat(AT_FDCWD, "/proc/curproc/ns/pid", buf, PATH_MAX)
#endif
	    ;

	/* namespace string is expected to be like pid:[4026531836] */
	if (linklen > 0) {
		/* Trim the trailing "]" */
		buf[linklen - 1] = '\0';
		char *index = strtok(buf, "pid:[");
		ret = atol(index);
	}

	return ret;
}

JEMALLOC_ALWAYS_INLINE int
os_prof_open_maps(void) {
	int mfd;

#if defined(O_CLOEXEC)
	mfd = open("/proc/curproc/map", O_RDONLY | O_CLOEXEC);
#else
	mfd = open("/proc/curproc/map", O_RDONLY);
	if (mfd != -1) {
		fcntl(mfd, F_SETFD, fcntl(mfd, F_GETFD) | FD_CLOEXEC);
	}
#endif

	return mfd;
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_prof_dump_read_maps_cb(void *read_cbopaque, void *buf, size_t limit) {
	int mfd = *(int *)read_cbopaque;
	assert(mfd != -1);
	return malloc_read_fd(mfd, buf, limit);
}

JEMALLOC_ALWAYS_INLINE void
os_prof_dump_maps(buf_writer_t *buf_writer, int (*open_maps)(void)) {
	int mfd = open_maps();
	if (mfd == -1) {
		return;
	}

	buf_writer_cb(buf_writer, "\nMAPPED_LIBRARIES:\n");
	buf_writer_pipe(buf_writer, os_prof_dump_read_maps_cb, &mfd);
	close(mfd);
}

#endif /* JEMALLOC_INTERNAL_OS_FREEBSD_PROC_MAPS_H */
