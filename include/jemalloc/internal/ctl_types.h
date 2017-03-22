#ifndef JEMALLOC_INTERNAL_CTL_TYPES_H
#define JEMALLOC_INTERNAL_CTL_TYPES_H

#define GLOBAL_PROF_MUTEXES						\
    OP(ctl)								\
    OP(prof)

typedef enum {
#define OP(mtx) global_prof_mutex_##mtx,
	GLOBAL_PROF_MUTEXES
#undef OP
	num_global_prof_mutexes
} global_prof_mutex_ind_t;

#define ARENA_PROF_MUTEXES						\
    OP(large)								\
    OP(extent_freelist)							\
    OP(extents_dirty)							\
    OP(extents_muzzy)							\
    OP(extents_retained)						\
    OP(decay_dirty)							\
    OP(decay_muzzy)							\
    OP(base)								\
    OP(tcache_list)

typedef enum {
#define OP(mtx) arena_prof_mutex_##mtx,
	ARENA_PROF_MUTEXES
#undef OP
	num_arena_prof_mutexes
} arena_prof_mutex_ind_t;

#define MUTEX_PROF_COUNTERS						\
    OP(num_ops, uint64_t)						\
    OP(num_wait, uint64_t)						\
    OP(num_spin_acq, uint64_t)						\
    OP(num_owner_switch, uint64_t)					\
    OP(total_wait_time, uint64_t)					\
    OP(max_wait_time, uint64_t)						\
    OP(max_num_thds, uint32_t)

typedef enum {
#define OP(counter, type) mutex_counter_##counter,
	MUTEX_PROF_COUNTERS
#undef OP
	num_mutex_prof_counters
} mutex_prof_counter_ind_t;

typedef struct ctl_node_s ctl_node_t;
typedef struct ctl_named_node_s ctl_named_node_t;
typedef struct ctl_indexed_node_s ctl_indexed_node_t;
typedef struct ctl_arena_stats_s ctl_arena_stats_t;
typedef struct ctl_stats_s ctl_stats_t;
typedef struct ctl_arena_s ctl_arena_t;
typedef struct ctl_arenas_s ctl_arenas_t;

#endif /* JEMALLOC_INTERNAL_CTL_TYPES_H */
