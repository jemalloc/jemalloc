#ifndef TEST_HPA_H
#define TEST_HPA_H

/*
 * Static in production builds (JET_EXTERN); exported only for the unit tests
 * that share it (hpa_central, psset).
 */
extern bool hpa_hugepage_size_exceeds_limit(void);

#endif /* TEST_HPA_H */
