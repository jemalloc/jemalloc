#ifndef JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H
#define JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

#include <mach-o/dyld.h>

#define JEMALLOC_OS_PROF_NO_OPEN_MAPS
#define JEMALLOC_OS_PROF_MAPS_SNAPSHOT

#ifdef __LP64__
typedef struct mach_header_64     os_mach_header_t;
typedef struct segment_command_64 os_segment_command_t;
#	define OS_MH_MAGIC_VALUE MH_MAGIC_64
#	define OS_MH_CIGAM_VALUE MH_CIGAM_64
#	define OS_LC_SEGMENT_VALUE LC_SEGMENT_64
#else
typedef struct mach_header     os_mach_header_t;
typedef struct segment_command os_segment_command_t;
#	define OS_MH_MAGIC_VALUE MH_MAGIC
#	define OS_MH_CIGAM_VALUE MH_CIGAM
#	define OS_LC_SEGMENT_VALUE LC_SEGMENT
#endif

/*
 * Longest "%016llx-%016llx: %s\n" line, excluding the terminating NUL: 36
 * bytes of fixed text plus a path of at most PATH_MAX bytes.
 */
#define OS_PROF_MAPS_ENTRY_MAX (PATH_MAX + 36)

JEMALLOC_ALWAYS_INLINE uint64_t
os_prof_pid_namespace(void) {
	/* Not supported on Darwin. */
	return 0;
}

JEMALLOC_ALWAYS_INLINE int
os_prof_open_maps(void) {
	/* Darwin dumps dyld images instead of reading an fd-based maps file. */
	return -1;
}

/*
 * Formats image_index's __TEXT mapping into buf, which must have room for
 * OS_PROF_MAPS_ENTRY_MAX + 1 bytes.  Returns the number of bytes written,
 * excluding the terminating NUL.
 */
JEMALLOC_ALWAYS_INLINE size_t
os_prof_dyld_image_vmaddr(char *buf, uint32_t image_index) {
	const os_mach_header_t *header = (const os_mach_header_t *)
	    _dyld_get_image_header(image_index);
	if (header == NULL
	    || (header->magic != OS_MH_MAGIC_VALUE
	        && header->magic != OS_MH_CIGAM_VALUE)) {
		/* Invalid header. */
		return 0;
	}

	intptr_t             slide = _dyld_get_image_vmaddr_slide(image_index);
	const char          *name = _dyld_get_image_name(image_index);
	struct load_command *load_cmd = (struct load_command *)((char *)header
	    + sizeof(os_mach_header_t));
	for (uint32_t i = 0; load_cmd && (i < header->ncmds); i++) {
		if (load_cmd->cmd == OS_LC_SEGMENT_VALUE) {
			const os_segment_command_t *segment_cmd =
			    (const os_segment_command_t *)load_cmd;
			if (!strcmp(segment_cmd->segname, "__TEXT")) {
				size_t len = malloc_snprintf(buf,
				    OS_PROF_MAPS_ENTRY_MAX + 1,
				    "%016llx-%016llx: %s\n",
				    segment_cmd->vmaddr + slide,
				    segment_cmd->vmaddr + slide
				        + segment_cmd->vmsize,
				    name);
				return len > OS_PROF_MAPS_ENTRY_MAX
				    ? OS_PROF_MAPS_ENTRY_MAX
				    : len;
			}
		}
		load_cmd = (struct load_command *)((char *)load_cmd
		    + load_cmd->cmdsize);
	}
	return 0;
}

JEMALLOC_ALWAYS_INLINE size_t
os_prof_maps_snapshot_size(void) {
	return (size_t)_dyld_image_count() * OS_PROF_MAPS_ENTRY_MAX + 1;
}

JEMALLOC_ALWAYS_INLINE size_t
os_prof_maps_snapshot(char *buf, size_t size) {
	uint32_t image_count = _dyld_image_count();
	size_t   len = 0;
	for (uint32_t i = 0; i < image_count; i++) {
		/* Images may have been loaded since size was computed. */
		if (size - len < OS_PROF_MAPS_ENTRY_MAX + 1) {
			break;
		}
		len += os_prof_dyld_image_vmaddr(buf + len, i);
	}
	buf[len] = '\0';
	return len;
}

/*
 * No proc map file to read on Darwin; the dyld image walk happens up front in
 * os_prof_maps_snapshot() instead, because dyld must not be called under
 * prof_dump_mtx.  open_maps is never called here, and maps is NULL if the
 * snapshot could not be allocated.
 */
JEMALLOC_ALWAYS_INLINE void
os_prof_dump_maps(buf_writer_t *buf_writer, int (*open_maps)(void),
    const char *maps) {
	(void)open_maps;
	buf_writer_cb(buf_writer, "\nMAPPED_LIBRARIES:\n");
	if (maps != NULL) {
		buf_writer_cb(buf_writer, maps);
	}
}

#endif /* JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H */
