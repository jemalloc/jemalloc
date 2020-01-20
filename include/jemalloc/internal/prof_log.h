#ifndef JEMALLOC_INTERNAL_PROF_LOG_EXTERNS_H
#define JEMALLOC_INTERNAL_PROF_LOG_EXTERNS_H

#include "jemalloc/internal/mutex.h"

extern malloc_mutex_t log_mtx;

void prof_try_log(tsd_t *tsd, size_t usize, prof_info_t *prof_info);
bool prof_log_init(tsd_t *tsdn);
#ifdef JEMALLOC_JET
size_t prof_log_bt_count(void);
size_t prof_log_alloc_count(void);
size_t prof_log_thr_count(void);
bool prof_log_is_logging(void);
bool prof_log_rep_check(void);
void prof_log_dummy_set(bool new_value);
#endif

#endif /* JEMALLOC_INTERNAL_PROF_LOG_EXTERNS_H */
