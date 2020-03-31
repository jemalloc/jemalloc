#ifndef JEMALLOC_INTERNAL_PROF_DATA_H
#define JEMALLOC_INTERNAL_PROF_DATA_H

#include "jemalloc/internal/mutex.h"

extern malloc_mutex_t *gctx_locks;
extern malloc_mutex_t *tdata_locks;

void prof_bt_hash(const void *key, size_t r_hash[2]);
bool prof_bt_keycomp(const void *k1, const void *k2);

bool prof_data_init(tsd_t *tsd);
char *prof_thread_name_alloc(tsdn_t *tsdn, const char *thread_name);
int prof_thread_name_set_impl(tsd_t *tsd, const char *thread_name);
bool prof_dump(tsd_t *tsd, bool propagate_err, const char *filename,
    bool leakcheck);
prof_tdata_t * prof_tdata_init_impl(tsd_t *tsd, uint64_t thr_uid,
    uint64_t thr_discrim, char *thread_name, bool active);
void prof_tdata_detach(tsd_t *tsd, prof_tdata_t *tdata);
void bt_init(prof_bt_t *bt, void **vec);
void prof_backtrace(tsd_t *tsd, prof_bt_t *bt);
void prof_tctx_try_destroy(tsd_t *tsd, prof_tctx_t *tctx);

/* Used in unit tests. */
size_t prof_tdata_count(void);
size_t prof_bt_count(void);
typedef void (prof_dump_header_t)(tsdn_t *, const prof_cnt_t *);
extern prof_dump_header_t *JET_MUTABLE prof_dump_header;
void prof_cnt_all(uint64_t *curobjs, uint64_t *curbytes, uint64_t *accumobjs,
    uint64_t *accumbytes);

#endif /* JEMALLOC_INTERNAL_PROF_DATA_H */
