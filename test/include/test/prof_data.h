#ifndef TEST_PROF_DATA_H
#define TEST_PROF_DATA_H

/*
 * Static in production builds (#ifdef JEMALLOC_JET); exported only for the unit
 * tests that share them (prof_reset, prof_accum, prof_active, prof_tctx).
 */
extern size_t prof_tdata_count(void);
extern size_t prof_bt_count(void);
extern void   prof_cnt_all(prof_cnt_t *cnt_all);

#endif /* TEST_PROF_DATA_H */
