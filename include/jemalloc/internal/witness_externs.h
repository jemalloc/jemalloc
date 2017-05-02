#ifndef JEMALLOC_INTERNAL_WITNESS_EXTERNS_H
#define JEMALLOC_INTERNAL_WITNESS_EXTERNS_H

void witness_init(witness_t *witness, const char *name, witness_rank_t rank,
    witness_comp_t *comp, void *opaque);

typedef void (witness_lock_error_t)(const witness_list_t *, const witness_t *);
extern witness_lock_error_t *JET_MUTABLE witness_lock_error;

typedef void (witness_owner_error_t)(const witness_t *);
extern witness_owner_error_t *JET_MUTABLE witness_owner_error;

typedef void (witness_not_owner_error_t)(const witness_t *);
extern witness_not_owner_error_t *JET_MUTABLE witness_not_owner_error;

typedef void (witness_depth_error_t)(const witness_list_t *,
    witness_rank_t rank_inclusive, unsigned depth);
extern witness_depth_error_t *JET_MUTABLE witness_depth_error;

void witnesses_cleanup(tsd_t *tsd);
void witness_prefork(tsd_t *tsd);
void witness_postfork_parent(tsd_t *tsd);
void witness_postfork_child(tsd_t *tsd);

#endif /* JEMALLOC_INTERNAL_WITNESS_EXTERNS_H */
