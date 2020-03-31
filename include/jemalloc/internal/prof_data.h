#ifndef JEMALLOC_INTERNAL_PROF_DATA_H
#define JEMALLOC_INTERNAL_PROF_DATA_H

#include "jemalloc/internal/mutex.h"

extern malloc_mutex_t *gctx_locks;
extern malloc_mutex_t *tdata_locks;

void prof_bt_hash(const void *key, size_t r_hash[2]);
bool prof_bt_keycomp(const void *k1, const void *k2);

bool prof_data_init(tsd_t *tsd);
bool prof_dump(tsd_t *tsd, bool propagate_err, const char *filename,
    bool leakcheck);
prof_tdata_t * prof_tdata_init_impl(tsd_t *tsd, uint64_t thr_uid,
    uint64_t thr_discrim, char *thread_name, bool active);
void prof_tdata_detach(tsd_t *tsd, prof_tdata_t *tdata);
void bt_init(prof_bt_t *bt, void **vec);
void prof_backtrace(tsd_t *tsd, prof_bt_t *bt);
void prof_tctx_try_destroy(tsd_t *tsd, prof_tctx_t *tctx);

#endif /* JEMALLOC_INTERNAL_PROF_DATA_H */
