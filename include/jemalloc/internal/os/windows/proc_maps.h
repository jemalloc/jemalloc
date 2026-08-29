#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_PROC_MAPS_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_PROC_MAPS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

JEMALLOC_ALWAYS_INLINE uint64_t
os_prof_pid_namespace(void) {
	/* Not supported on Windows. */
	return 0;
}

JEMALLOC_ALWAYS_INLINE int
os_prof_open_maps(void) {
	/* Not implemented on Windows. */
	return -1;
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_prof_dump_read_maps_cb(void *read_cbopaque, void *buf, size_t limit) {
	int mfd = *(int *)read_cbopaque;
	assert(mfd != -1);
	return malloc_read_fd(mfd, buf, limit);
}

JEMALLOC_ALWAYS_INLINE void
os_prof_dump_maps(buf_writer_t *buf_writer, int (*open_maps)(void)) {
	/*
	 * With the default Windows hook, the rest of this function is
	 * unreachable.
	 */
	int mfd = open_maps();
	if (mfd == -1) {
		return;
	}

	buf_writer_cb(buf_writer, "\nMAPPED_LIBRARIES:\n");
	buf_writer_pipe(buf_writer, os_prof_dump_read_maps_cb, &mfd);
	close(mfd);
}

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_PROC_MAPS_H */
