#ifndef JEMALLOC_INTERNAL_NSTIME_TYPES_H
#define JEMALLOC_INTERNAL_NSTIME_TYPES_H

typedef struct nstime_s nstime_t;

/* Maximum supported number of seconds (~584 years). */
#define NSTIME_SEC_MAX	KQU(18446744072)

#define NSTIME_ZERO_INITIALIZER {0}

#endif /* JEMALLOC_INTERNAL_NSTIME_TYPES_H */
