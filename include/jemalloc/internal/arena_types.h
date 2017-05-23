#ifndef JEMALLOC_INTERNAL_ARENA_TYPES_H
#define JEMALLOC_INTERNAL_ARENA_TYPES_H

#define LARGE_MINCLASS		(ZU(1) << LG_LARGE_MINCLASS)

/* Maximum number of regions in one slab. */
#define LG_SLAB_MAXREGS		(LG_PAGE - LG_TINY_MIN)
#define SLAB_MAXREGS		(1U << LG_SLAB_MAXREGS)

/* Default decay times in milliseconds. */
#define DIRTY_DECAY_MS_DEFAULT	ZD(10 * 1000)
#define MUZZY_DECAY_MS_DEFAULT	ZD(10 * 1000)
/* Number of event ticks between time checks. */
#define DECAY_NTICKS_PER_UPDATE	1000

typedef struct arena_slab_data_s arena_slab_data_t;
typedef struct arena_bin_info_s arena_bin_info_t;
typedef struct arena_decay_s arena_decay_t;
typedef struct arena_bin_s arena_bin_t;
typedef struct arena_s arena_t;
typedef struct arena_tdata_s arena_tdata_t;
typedef struct alloc_ctx_s alloc_ctx_t;

typedef enum {
	percpu_arena_disabled = 0,
	percpu_arena = 1,
	per_phycpu_arena = 2, /* i.e. hyper threads share arena. */

	percpu_arena_mode_limit = 3
} percpu_arena_mode_t;

#define PERCPU_ARENA_MODE_DEFAULT	percpu_arena_disabled
#define OPT_PERCPU_ARENA_DEFAULT	"disabled"

#endif /* JEMALLOC_INTERNAL_ARENA_TYPES_H */
