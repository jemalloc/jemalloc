#ifndef JEMALLOC_INTERNAL_PROF_LOG_H
#define JEMALLOC_INTERNAL_PROF_LOG_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/mutex.h"

extern malloc_mutex_t log_mtx;

void prof_try_log(tsd_t *tsd, size_t usize, prof_info_t *prof_info);
bool prof_log_init(tsd_t *tsdn);

bool prof_log_start(tsdn_t *tsdn, const char *filename);
bool prof_log_stop(tsdn_t *tsdn);

#endif /* JEMALLOC_INTERNAL_PROF_LOG_H */
