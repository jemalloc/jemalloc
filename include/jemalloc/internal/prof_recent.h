#ifndef JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H
#define JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H

bool prof_recent_alloc_prepare(tsd_t *tsd, prof_tctx_t *tctx);
void prof_recent_alloc(tsd_t *tsd, edata_t *edata, size_t usize);
void prof_recent_alloc_reset(tsd_t *tsd, edata_t *edata);
bool prof_recent_init();
void edata_prof_recent_alloc_init(edata_t *edata);
#ifdef JEMALLOC_JET
prof_recent_t *prof_recent_alloc_begin(tsd_t *tsd);
prof_recent_t *prof_recent_alloc_end(tsd_t *tsd);
prof_recent_t *prof_recent_alloc_next(tsd_t *tsd, prof_recent_t *node);
prof_recent_t *edata_prof_recent_alloc_get(tsd_t *tsd, const edata_t *edata);
#endif

#endif /* JEMALLOC_INTERNAL_PROF_RECENT_EXTERNS_H */
