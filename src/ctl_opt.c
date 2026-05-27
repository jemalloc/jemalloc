#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/extent.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/prof.h"

/******************************************************************************/
/* opt.* mallctl handlers. */

CTL_RO_NL_GEN_PUBLIC(opt_abort, opt_abort, bool)
CTL_RO_NL_GEN_PUBLIC(opt_abort_conf, opt_abort_conf, bool)
CTL_RO_NL_GEN_PUBLIC(opt_cache_oblivious, opt_cache_oblivious, bool)
CTL_RO_NL_GEN_PUBLIC(
    opt_debug_double_free_max_scan, opt_debug_double_free_max_scan, unsigned)
CTL_RO_NL_GEN_PUBLIC(opt_trust_madvise, opt_trust_madvise, bool)
CTL_RO_NL_GEN_PUBLIC(opt_experimental_hpa_start_huge_if_thp_always,
    opt_experimental_hpa_start_huge_if_thp_always, bool)
CTL_RO_NL_GEN_PUBLIC(opt_experimental_hpa_enforce_hugify,
    opt_experimental_hpa_enforce_hugify, bool)
CTL_RO_NL_GEN_PUBLIC(opt_confirm_conf, opt_confirm_conf, bool)

/* HPA options. */
CTL_RO_NL_GEN_PUBLIC(opt_hpa, opt_hpa, bool)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_hugification_threshold, opt_hpa_opts.hugification_threshold, size_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_hugify_delay_ms, opt_hpa_opts.hugify_delay_ms, uint64_t)
CTL_RO_NL_GEN_PUBLIC(opt_hpa_hugify_sync, opt_hpa_opts.hugify_sync, bool)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_min_purge_interval_ms, opt_hpa_opts.min_purge_interval_ms, uint64_t)
CTL_RO_NL_GEN_PUBLIC(opt_experimental_hpa_max_purge_nhp,
    opt_hpa_opts.experimental_max_purge_nhp, ssize_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_purge_threshold, opt_hpa_opts.purge_threshold, size_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_min_purge_delay_ms, opt_hpa_opts.min_purge_delay_ms, uint64_t)
CTL_RO_NL_GEN_PUBLIC(opt_hpa_hugify_style,
    hpa_hugify_style_names[opt_hpa_opts.hugify_style], const char *)
/*
 * This will have to change before we publicly document this option; fxp_t and
 * its representation are internal implementation details.
 */
CTL_RO_NL_GEN_PUBLIC(opt_hpa_dirty_mult, opt_hpa_opts.dirty_mult, fxp_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_hpa_slab_max_alloc, opt_hpa_opts.slab_max_alloc, size_t)

/* HPA SEC options */
CTL_RO_NL_GEN_PUBLIC(opt_hpa_sec_nshards, opt_hpa_sec_opts.nshards, size_t)
CTL_RO_NL_GEN_PUBLIC(opt_hpa_sec_max_alloc, opt_hpa_sec_opts.max_alloc, size_t)
CTL_RO_NL_GEN_PUBLIC(opt_hpa_sec_max_bytes, opt_hpa_sec_opts.max_bytes, size_t)
CTL_RO_NL_GEN_PUBLIC(opt_huge_arena_pac_thp, opt_huge_arena_pac_thp, bool)
CTL_RO_NL_GEN_PUBLIC(
    opt_metadata_thp, metadata_thp_mode_names[opt_metadata_thp], const char *)
CTL_RO_NL_GEN_PUBLIC(opt_retain, opt_retain, bool)
CTL_RO_NL_GEN_PUBLIC(opt_dss, opt_dss, const char *)
CTL_RO_NL_GEN_PUBLIC(opt_narenas, opt_narenas, unsigned)
CTL_RO_NL_GEN_PUBLIC(
    opt_percpu_arena, percpu_arena_mode_names[opt_percpu_arena], const char *)
CTL_RO_NL_GEN_PUBLIC(opt_mutex_max_spin, opt_mutex_max_spin, int64_t)
CTL_RO_NL_GEN_PUBLIC(opt_oversize_threshold, opt_oversize_threshold, size_t)
CTL_RO_NL_GEN_PUBLIC(opt_background_thread, opt_background_thread, bool)
CTL_RO_NL_GEN_PUBLIC(opt_max_background_threads, opt_max_background_threads,
    size_t)
CTL_RO_NL_GEN_PUBLIC(opt_dirty_decay_ms, opt_dirty_decay_ms, ssize_t)
CTL_RO_NL_GEN_PUBLIC(opt_muzzy_decay_ms, opt_muzzy_decay_ms, ssize_t)
CTL_RO_NL_GEN_PUBLIC(opt_stats_print, opt_stats_print, bool)
CTL_RO_NL_GEN_PUBLIC(opt_stats_print_opts, opt_stats_print_opts, const char *)
CTL_RO_NL_GEN_PUBLIC(opt_stats_interval, opt_stats_interval, int64_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_stats_interval_opts, opt_stats_interval_opts, const char *)
CTL_RO_NL_CGEN_PUBLIC(config_fill, opt_junk, opt_junk, const char *)
CTL_RO_NL_CGEN_PUBLIC(config_fill, opt_zero, opt_zero, bool)
CTL_RO_NL_CGEN_PUBLIC(config_utrace, opt_utrace, opt_utrace, bool)
CTL_RO_NL_CGEN_PUBLIC(config_xmalloc, opt_xmalloc, opt_xmalloc, bool)
CTL_RO_NL_GEN_PUBLIC(
    opt_experimental_tcache_gc, opt_experimental_tcache_gc, bool)
CTL_RO_NL_GEN_PUBLIC(opt_tcache, opt_tcache, bool)
CTL_RO_NL_GEN_PUBLIC(opt_tcache_max, opt_tcache_max, size_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_tcache_nslots_small_min, opt_tcache_nslots_small_min, unsigned)
CTL_RO_NL_GEN_PUBLIC(
    opt_tcache_nslots_small_max, opt_tcache_nslots_small_max, unsigned)
CTL_RO_NL_GEN_PUBLIC(opt_tcache_nslots_large, opt_tcache_nslots_large, unsigned)
CTL_RO_NL_GEN_PUBLIC(
    opt_lg_tcache_nslots_mul, opt_lg_tcache_nslots_mul, ssize_t)
CTL_RO_NL_GEN_PUBLIC(opt_tcache_gc_incr_bytes, opt_tcache_gc_incr_bytes, size_t)
CTL_RO_NL_GEN_PUBLIC(opt_tcache_gc_delay_bytes, opt_tcache_gc_delay_bytes,
    size_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_lg_tcache_flush_small_div, opt_lg_tcache_flush_small_div, unsigned)
CTL_RO_NL_GEN_PUBLIC(
    opt_lg_tcache_flush_large_div, opt_lg_tcache_flush_large_div, unsigned)
CTL_RO_NL_GEN_PUBLIC(opt_thp, thp_mode_names[opt_thp], const char *)
CTL_RO_NL_GEN_PUBLIC(
    opt_lg_extent_max_active_fit, opt_lg_extent_max_active_fit, size_t)
CTL_RO_NL_GEN_PUBLIC(
    opt_process_madvise_max_batch, opt_process_madvise_max_batch, size_t)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof, opt_prof, bool)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_prefix, opt_prof_prefix,
    const char *)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_active, opt_prof_active, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_prof_thread_active_init, opt_prof_thread_active_init, bool)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_bt_max, opt_prof_bt_max, unsigned)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_lg_prof_sample, opt_lg_prof_sample, size_t)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_accum, opt_prof_accum, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_prof_pid_namespace, opt_prof_pid_namespace, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_lg_prof_interval, opt_lg_prof_interval, ssize_t)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_gdump, opt_prof_gdump, bool)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_final, opt_prof_final, bool)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_leak, opt_prof_leak, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_prof_leak_error, opt_prof_leak_error, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_prof_recent_alloc_max, opt_prof_recent_alloc_max, ssize_t)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_stats, opt_prof_stats, bool)
CTL_RO_NL_CGEN_PUBLIC(
    config_prof, opt_prof_sys_thread_name, opt_prof_sys_thread_name, bool)
CTL_RO_NL_CGEN_PUBLIC(config_prof, opt_prof_time_res,
    prof_time_res_mode_names[opt_prof_time_res], const char *)
CTL_RO_NL_CGEN_PUBLIC(
    config_uaf_detection, opt_lg_san_uaf_align, opt_lg_san_uaf_align, ssize_t)
CTL_RO_NL_GEN_PUBLIC(opt_zero_realloc,
    zero_realloc_mode_names[opt_zero_realloc_action], const char *)
CTL_RO_NL_GEN_PUBLIC(
    opt_disable_large_size_classes, opt_disable_large_size_classes, bool)

/* malloc_conf options */
CTL_RO_NL_CGEN_PUBLIC(opt_malloc_conf_symlink, opt_malloc_conf_symlink,
    opt_malloc_conf_symlink, const char *)
CTL_RO_NL_CGEN_PUBLIC(opt_malloc_conf_env_var, opt_malloc_conf_env_var,
    opt_malloc_conf_env_var, const char *)
CTL_RO_NL_CGEN_PUBLIC(
    je_malloc_conf, opt_malloc_conf_global_var, je_malloc_conf, const char *)
