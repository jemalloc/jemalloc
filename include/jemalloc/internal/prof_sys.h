#ifndef JEMALLOC_INTERNAL_PROF_SYS_H
#define JEMALLOC_INTERNAL_PROF_SYS_H

void prof_sys_thread_name_fetch(tsd_t *tsd);

/* Used in unit tests. */
typedef int (prof_sys_thread_name_read_t)(char *buf, size_t limit);
extern prof_sys_thread_name_read_t *JET_MUTABLE prof_sys_thread_name_read;

#endif /* JEMALLOC_INTERNAL_PROF_SYS_H */
