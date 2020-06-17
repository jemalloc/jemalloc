#ifndef JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H
#define JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H

extern malloc_mutex_t prof_recent_dump_mtx;

bool prof_recent_alloc_prepare(tsd_t *tsd, prof_tctx_t *tctx);
void prof_recent_alloc(tsd_t *tsd, edata_t *edata, size_t size);
void prof_recent_alloc_reset(tsd_t *tsd, edata_t *edata);
bool prof_recent_init();
void edata_prof_recent_alloc_init(edata_t *edata);

/* Used in unit tests. */
typedef ql_head(prof_recent_t) prof_recent_list_t;
extern prof_recent_list_t prof_recent_alloc_list;
edata_t *prof_recent_alloc_edata_get_no_lock_test(const prof_recent_t *node);
prof_recent_t *edata_prof_recent_alloc_get_no_lock_test(const edata_t *edata);

#endif /* JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H */
