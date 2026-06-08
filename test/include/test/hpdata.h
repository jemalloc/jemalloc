#ifndef TEST_HPDATA_H
#define TEST_HPDATA_H

/*
 * Static in production builds (JET_EXTERN); exported only for the unit tests
 * that share it (hpdata, psset).
 */
extern void *hpdata_reserve_alloc(hpdata_t *hpdata, size_t sz);

#endif /* TEST_HPDATA_H */
