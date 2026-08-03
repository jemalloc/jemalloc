#ifndef JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H
#define JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/malloc_io.h"

#include <mach-o/dyld.h>

#define JEMALLOC_OS_PROF_NO_OPEN_MAPS

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

JEMALLOC_ALWAYS_INLINE long
os_prof_pid_namespace(void) {
	/* Not supported on Darwin. */
	return 0;
}

JEMALLOC_ALWAYS_INLINE int
os_prof_open_maps(void) {
	/* Darwin dumps dyld images instead of reading an fd-based maps file. */
	return -1;
}

JEMALLOC_ALWAYS_INLINE void
os_prof_dump_dyld_image_vmaddr(buf_writer_t *buf_writer, uint32_t image_index) {
	const os_mach_header_t *header = (const os_mach_header_t *)
	    _dyld_get_image_header(image_index);
	if (header == NULL
	    || (header->magic != OS_MH_MAGIC_VALUE
	        && header->magic != OS_MH_CIGAM_VALUE)) {
		/* Invalid header. */
		return;
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
				char buffer[PATH_MAX + 1];
				malloc_snprintf(buffer, sizeof(buffer),
				    "%016llx-%016llx: %s\n",
				    segment_cmd->vmaddr + slide,
				    segment_cmd->vmaddr + slide
				        + segment_cmd->vmsize,
				    name);
				buf_writer_cb(buf_writer, buffer);
				return;
			}
		}
		load_cmd = (struct load_command *)((char *)load_cmd
		    + load_cmd->cmdsize);
	}
}

/*
 * No proc map file to read on Darwin, so os_prof_dump_maps() walks
 * dyld-loaded images for backtrace instead.  open_maps is never called here.
 */
JEMALLOC_ALWAYS_INLINE void
os_prof_dump_maps(buf_writer_t *buf_writer, int (*open_maps)(void)) {
	(void)open_maps;
	buf_writer_cb(buf_writer, "\nMAPPED_LIBRARIES:\n");
	uint32_t image_count = _dyld_image_count();
	for (uint32_t i = 0; i < image_count; i++) {
		os_prof_dump_dyld_image_vmaddr(buf_writer, i);
	}
}

#endif /* JEMALLOC_INTERNAL_OS_DARWIN_PROC_MAPS_H */
