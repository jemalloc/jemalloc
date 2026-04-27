#ifndef JEMALLOC_INTERNAL_EXTERNS_H
#define JEMALLOC_INTERNAL_EXTERNS_H

#include "jemalloc/internal/arena_types.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/fxp.h"
#include "jemalloc/internal/hpa_opts.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/sec_opts.h"
#include "jemalloc/internal/tsd_types.h"

/* TSD checks this to set thread local slow state accordingly. */
extern bool malloc_slow;

/* Run-time options. */
extern bool             opt_abort;
extern bool             opt_abort_conf;
extern bool             opt_trust_madvise;
extern bool             opt_experimental_hpa_start_huge_if_thp_always;
extern bool             opt_experimental_hpa_enforce_hugify;
extern bool             opt_confirm_conf;
extern bool             opt_hpa;
extern hpa_shard_opts_t opt_hpa_opts;
extern sec_opts_t       opt_hpa_sec_opts;

extern const char *opt_junk;
extern bool        opt_junk_alloc;
extern bool        opt_junk_free;
extern void (*JET_MUTABLE junk_free_callback)(void *ptr, size_t size);
extern void (*JET_MUTABLE junk_alloc_callback)(void *ptr, size_t size);
extern void (*JET_MUTABLE invalid_conf_abort)(void);
extern bool                  opt_utrace;
extern bool                  opt_xmalloc;
extern bool                  opt_experimental_infallible_new;
extern bool                  opt_experimental_tcache_gc;
extern bool                  opt_zero;
extern unsigned              opt_narenas;
extern fxp_t                 opt_narenas_ratio;
extern zero_realloc_action_t opt_zero_realloc_action;
extern malloc_init_t         malloc_init_state;
extern const char *const     zero_realloc_mode_names[];
extern atomic_zu_t           zero_realloc_count;
extern bool                  opt_cache_oblivious;
extern unsigned              opt_debug_double_free_max_scan;
extern size_t                opt_calloc_madvise_threshold;
extern bool                  opt_disable_large_size_classes;

extern const char *opt_malloc_conf_symlink;
extern const char *opt_malloc_conf_env_var;

/* Escape free-fastpath when ptr & mask == 0 (for sanitization purpose). */
extern uintptr_t san_cache_bin_nonfast_mask;

/* Number of CPUs. */
extern unsigned ncpus;

/* Will be refactored in subsequent commit */
bool malloc_init_hard_a0(void);

void    *bootstrap_malloc(size_t size);
void    *bootstrap_calloc(size_t num, size_t size);
void     bootstrap_free(void *ptr);
size_t   batch_alloc(void **ptrs, size_t num, size_t size, int flags);
void     jemalloc_prefork(void);
void     jemalloc_postfork_parent(void);
void     jemalloc_postfork_child(void);
void     sdallocx_default(void *ptr, size_t size, int flags);
void     free_default(void *ptr);
void    *malloc_default(size_t size);

#endif /* JEMALLOC_INTERNAL_EXTERNS_H */
