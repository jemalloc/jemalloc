#include "jemalloc/internal/jemalloc_preamble.h"

#include "ctl_externs.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/ctl_arena.h"
#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/ctl_stats.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/
/* Data. */

/*
 * ctl_mtx protects ctl stats state and arena ctl state.
 */
static malloc_mutex_t ctl_mtx;
static bool           ctl_initialized;

/******************************************************************************/
/* Helpers for named and indexed nodes. */

static const ctl_named_node_t *
ctl_named_node(const ctl_node_t *node) {
	return ((node->named) ? (const ctl_named_node_t *)node : NULL);
}

static const ctl_named_node_t *
ctl_named_children(const ctl_named_node_t *node, size_t index) {
	const ctl_named_node_t *children = ctl_named_node(node->children);

	return (children ? &children[index] : NULL);
}

static const ctl_indexed_node_t *
ctl_indexed_node(const ctl_node_t *node) {
	return (!node->named ? (const ctl_indexed_node_t *)node : NULL);
}

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

#define CTL_PROTO(n)                                                           \
	static int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,       \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen);

#define INDEX_PROTO(n)                                                         \
	static const ctl_named_node_t *n##_index(                              \
	    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i);

CTL_PROTO(version)
CTL_PROTO(epoch)
INDEX_PROTO(arena_i)
INDEX_PROTO(arenas_bin_i)
INDEX_PROTO(arenas_lextent_i)
INDEX_PROTO(stats_arenas_i_bins_j)
INDEX_PROTO(stats_arenas_i_lextents_j)
INDEX_PROTO(stats_arenas_i_extents_j)
INDEX_PROTO(stats_arenas_i_hpa_shard_nonfull_slabs_j)
INDEX_PROTO(stats_arenas_i_hpa_shard_alloc_j)
INDEX_PROTO(stats_arenas_i)
INDEX_PROTO(prof_stats_bins_i)
INDEX_PROTO(prof_stats_lextents_i)
/******************************************************************************/
/*
 * mallctl tree.
 *
 * Handler implementations live in ctl_* namespace modules. Indexed-node
 * callbacks stay here because they navigate this tree and return tree nodes.
 */

#define NAME(n) {true}, n
#define CHILD(t, c)                                                            \
	sizeof(c##_node) / sizeof(ctl_##t##_node_t), (ctl_node_t *)c##_node,   \
	    NULL
#define CTL(c) 0, NULL, c##_ctl

/*
 * Only handles internal indexed nodes, since there are currently no external
 * ones.
 */
#define INDEX(i) {false}, i##_index

static const ctl_named_node_t thread_tcache_ncached_max_node[] = {
    {NAME("read_sizeclass"), CTL(thread_tcache_ncached_max_read_sizeclass)},
    {NAME("write"), CTL(thread_tcache_ncached_max_write)}};

static const ctl_named_node_t thread_tcache_node[] = {
    {NAME("enabled"), CTL(thread_tcache_enabled)},
    {NAME("max"), CTL(thread_tcache_max)},
    {NAME("flush"), CTL(thread_tcache_flush)},
    {NAME("ncached_max"), CHILD(named, thread_tcache_ncached_max)}};

static const ctl_named_node_t thread_peak_node[] = {
    {NAME("read"), CTL(thread_peak_read)},
    {NAME("reset"), CTL(thread_peak_reset)},
};

static const ctl_named_node_t thread_prof_node[] = {
    {NAME("name"), CTL(thread_prof_name)},
    {NAME("active"), CTL(thread_prof_active)}};

static const ctl_named_node_t thread_node[] = {
    {NAME("arena"), CTL(thread_arena)},
    {NAME("allocated"), CTL(thread_allocated)},
    {NAME("allocatedp"), CTL(thread_allocatedp)},
    {NAME("deallocated"), CTL(thread_deallocated)},
    {NAME("deallocatedp"), CTL(thread_deallocatedp)},
    {NAME("tcache"), CHILD(named, thread_tcache)},
    {NAME("peak"), CHILD(named, thread_peak)},
    {NAME("prof"), CHILD(named, thread_prof)},
    {NAME("idle"), CTL(thread_idle)}};

static const ctl_named_node_t config_node[] = {
    {NAME("cache_oblivious"), CTL(config_cache_oblivious)},
    {NAME("debug"), CTL(config_debug)},
    {NAME("fill"), CTL(config_fill)},
    {NAME("infallible_new"), CTL(config_infallible_new)},
    {NAME("lazy_lock"), CTL(config_lazy_lock)},
    {NAME("malloc_conf"), CTL(config_malloc_conf)},
    {NAME("opt_safety_checks"), CTL(config_opt_safety_checks)},
    {NAME("prof"), CTL(config_prof)},
    {NAME("prof_libgcc"), CTL(config_prof_libgcc)},
    {NAME("prof_libunwind"), CTL(config_prof_libunwind)},
    {NAME("prof_frameptr"), CTL(config_prof_frameptr)},
    {NAME("stats"), CTL(config_stats)},
    {NAME("utrace"), CTL(config_utrace)},
    {NAME("xmalloc"), CTL(config_xmalloc)}};

static const ctl_named_node_t opt_malloc_conf_node[] = {
    {NAME("symlink"), CTL(opt_malloc_conf_symlink)},
    {NAME("env_var"), CTL(opt_malloc_conf_env_var)},
    {NAME("global_var"), CTL(opt_malloc_conf_global_var)}};

static const ctl_named_node_t opt_node[] = {{NAME("abort"), CTL(opt_abort)},
    {NAME("abort_conf"), CTL(opt_abort_conf)},
    {NAME("cache_oblivious"), CTL(opt_cache_oblivious)},
    {NAME("trust_madvise"), CTL(opt_trust_madvise)},
    {NAME("experimental_hpa_start_huge_if_thp_always"),
        CTL(opt_experimental_hpa_start_huge_if_thp_always)},
    {NAME("experimental_hpa_enforce_hugify"),
        CTL(opt_experimental_hpa_enforce_hugify)},
    {NAME("confirm_conf"), CTL(opt_confirm_conf)}, {NAME("hpa"), CTL(opt_hpa)},
    {NAME("hpa_slab_max_alloc"), CTL(opt_hpa_slab_max_alloc)},
    {NAME("hpa_hugification_threshold"), CTL(opt_hpa_hugification_threshold)},
    {NAME("hpa_hugify_delay_ms"), CTL(opt_hpa_hugify_delay_ms)},
    {NAME("hpa_hugify_sync"), CTL(opt_hpa_hugify_sync)},
    {NAME("hpa_min_purge_interval_ms"), CTL(opt_hpa_min_purge_interval_ms)},
    {NAME("experimental_hpa_max_purge_nhp"),
        CTL(opt_experimental_hpa_max_purge_nhp)},
    {NAME("hpa_purge_threshold"), CTL(opt_hpa_purge_threshold)},
    {NAME("hpa_min_purge_delay_ms"), CTL(opt_hpa_min_purge_delay_ms)},
    {NAME("hpa_hugify_style"), CTL(opt_hpa_hugify_style)},
    {NAME("hpa_dirty_mult"), CTL(opt_hpa_dirty_mult)},
    {NAME("hpa_sec_nshards"), CTL(opt_hpa_sec_nshards)},
    {NAME("hpa_sec_max_alloc"), CTL(opt_hpa_sec_max_alloc)},
    {NAME("hpa_sec_max_bytes"), CTL(opt_hpa_sec_max_bytes)},
    {NAME("huge_arena_pac_thp"), CTL(opt_huge_arena_pac_thp)},
    {NAME("metadata_thp"), CTL(opt_metadata_thp)},
    {NAME("retain"), CTL(opt_retain)}, {NAME("dss"), CTL(opt_dss)},
    {NAME("narenas"), CTL(opt_narenas)},
    {NAME("percpu_arena"), CTL(opt_percpu_arena)},
    {NAME("oversize_threshold"), CTL(opt_oversize_threshold)},
    {NAME("mutex_max_spin"), CTL(opt_mutex_max_spin)},
    {NAME("background_thread"), CTL(opt_background_thread)},
    {NAME("max_background_threads"), CTL(opt_max_background_threads)},
    {NAME("dirty_decay_ms"), CTL(opt_dirty_decay_ms)},
    {NAME("muzzy_decay_ms"), CTL(opt_muzzy_decay_ms)},
    {NAME("stats_print"), CTL(opt_stats_print)},
    {NAME("stats_print_opts"), CTL(opt_stats_print_opts)},
    {NAME("stats_interval"), CTL(opt_stats_interval)},
    {NAME("stats_interval_opts"), CTL(opt_stats_interval_opts)},
    {NAME("junk"), CTL(opt_junk)}, {NAME("zero"), CTL(opt_zero)},
    {NAME("utrace"), CTL(opt_utrace)}, {NAME("xmalloc"), CTL(opt_xmalloc)},
    {NAME("experimental_tcache_gc"), CTL(opt_experimental_tcache_gc)},
    {NAME("tcache"), CTL(opt_tcache)},
    {NAME("tcache_max"), CTL(opt_tcache_max)},
    {NAME("tcache_nslots_small_min"), CTL(opt_tcache_nslots_small_min)},
    {NAME("tcache_nslots_small_max"), CTL(opt_tcache_nslots_small_max)},
    {NAME("tcache_nslots_large"), CTL(opt_tcache_nslots_large)},
    {NAME("lg_tcache_nslots_mul"), CTL(opt_lg_tcache_nslots_mul)},
    {NAME("tcache_gc_incr_bytes"), CTL(opt_tcache_gc_incr_bytes)},
    {NAME("tcache_gc_delay_bytes"), CTL(opt_tcache_gc_delay_bytes)},
    {NAME("lg_tcache_flush_small_div"), CTL(opt_lg_tcache_flush_small_div)},
    {NAME("lg_tcache_flush_large_div"), CTL(opt_lg_tcache_flush_large_div)},
    {NAME("thp"), CTL(opt_thp)},
    {NAME("lg_extent_max_active_fit"), CTL(opt_lg_extent_max_active_fit)},
    {NAME("prof"), CTL(opt_prof)}, {NAME("prof_prefix"), CTL(opt_prof_prefix)},
    {NAME("prof_active"), CTL(opt_prof_active)},
    {NAME("prof_thread_active_init"), CTL(opt_prof_thread_active_init)},
    {NAME("prof_bt_max"), CTL(opt_prof_bt_max)},
    {NAME("lg_prof_sample"), CTL(opt_lg_prof_sample)},
    {NAME("lg_prof_interval"), CTL(opt_lg_prof_interval)},
    {NAME("prof_gdump"), CTL(opt_prof_gdump)},
    {NAME("prof_final"), CTL(opt_prof_final)},
    {NAME("prof_leak"), CTL(opt_prof_leak)},
    {NAME("prof_leak_error"), CTL(opt_prof_leak_error)},
    {NAME("prof_accum"), CTL(opt_prof_accum)},
    {NAME("prof_pid_namespace"), CTL(opt_prof_pid_namespace)},
    {NAME("prof_recent_alloc_max"), CTL(opt_prof_recent_alloc_max)},
    {NAME("prof_stats"), CTL(opt_prof_stats)},
    {NAME("prof_sys_thread_name"), CTL(opt_prof_sys_thread_name)},
    {NAME("prof_time_resolution"), CTL(opt_prof_time_res)},
    {NAME("lg_san_uaf_align"), CTL(opt_lg_san_uaf_align)},
    {NAME("zero_realloc"), CTL(opt_zero_realloc)},
    {NAME("debug_double_free_max_scan"), CTL(opt_debug_double_free_max_scan)},
    {NAME("disable_large_size_classes"), CTL(opt_disable_large_size_classes)},
    {NAME("process_madvise_max_batch"), CTL(opt_process_madvise_max_batch)},
    {NAME("malloc_conf"), CHILD(named, opt_malloc_conf)}};

static const ctl_named_node_t tcache_node[] = {
    {NAME("create"), CTL(tcache_create)},
    {NAME("flush"), CTL(tcache_flush)},
    {NAME("destroy"), CTL(tcache_destroy)}};

static const ctl_named_node_t arena_i_node[] = {
    {NAME("initialized"), CTL(arena_i_initialized)},
    {NAME("decay"), CTL(arena_i_decay)},
    {NAME("purge"), CTL(arena_i_purge)},
    {NAME("reset"), CTL(arena_i_reset)},
    {NAME("destroy"), CTL(arena_i_destroy)},
    {NAME("dss"), CTL(arena_i_dss)},
    /*
	 * Undocumented for now, since we anticipate an arena API in flux after
	 * we cut the last 5-series release.
	 */
    {NAME("oversize_threshold"), CTL(arena_i_oversize_threshold)},
    {NAME("dirty_decay_ms"), CTL(arena_i_dirty_decay_ms)},
    {NAME("muzzy_decay_ms"), CTL(arena_i_muzzy_decay_ms)},
    {NAME("extent_hooks"), CTL(arena_i_extent_hooks)},
    {NAME("retain_grow_limit"), CTL(arena_i_retain_grow_limit)},
    {NAME("name"), CTL(arena_i_name)}};
static const ctl_named_node_t super_arena_i_node[] = {
    {NAME(""), CHILD(named, arena_i)}};

static const ctl_indexed_node_t arena_node[] = {{INDEX(arena_i)}};

static const ctl_named_node_t arenas_bin_i_node[] = {
    {NAME("size"), CTL(arenas_bin_i_size)},
    {NAME("nregs"), CTL(arenas_bin_i_nregs)},
    {NAME("slab_size"), CTL(arenas_bin_i_slab_size)},
    {NAME("nshards"), CTL(arenas_bin_i_nshards)}};
static const ctl_named_node_t super_arenas_bin_i_node[] = {
    {NAME(""), CHILD(named, arenas_bin_i)}};

static const ctl_indexed_node_t arenas_bin_node[] = {{INDEX(arenas_bin_i)}};

static const ctl_named_node_t arenas_lextent_i_node[] = {
    {NAME("size"), CTL(arenas_lextent_i_size)}};
static const ctl_named_node_t super_arenas_lextent_i_node[] = {
    {NAME(""), CHILD(named, arenas_lextent_i)}};

static const ctl_indexed_node_t arenas_lextent_node[] = {
    {INDEX(arenas_lextent_i)}};

static const ctl_named_node_t arenas_node[] = {
    {NAME("narenas"), CTL(arenas_narenas)},
    {NAME("dirty_decay_ms"), CTL(arenas_dirty_decay_ms)},
    {NAME("muzzy_decay_ms"), CTL(arenas_muzzy_decay_ms)},
    {NAME("quantum"), CTL(arenas_quantum)},
    {NAME("page"), CTL(arenas_page)},
    {NAME("hugepage"), CTL(arenas_hugepage)},
    {NAME("tcache_max"), CTL(arenas_tcache_max)},
    {NAME("nbins"), CTL(arenas_nbins)},
    {NAME("nhbins"), CTL(arenas_nhbins)},
    {NAME("bin"), CHILD(indexed, arenas_bin)},
    {NAME("nlextents"), CTL(arenas_nlextents)},
    {NAME("lextent"), CHILD(indexed, arenas_lextent)},
    {NAME("create"), CTL(arenas_create)},
    {NAME("lookup"), CTL(arenas_lookup)}};

static const ctl_named_node_t prof_stats_bins_i_node[] = {
    {NAME("live"), CTL(prof_stats_bins_i_live)},
    {NAME("accum"), CTL(prof_stats_bins_i_accum)}};

static const ctl_named_node_t super_prof_stats_bins_i_node[] = {
    {NAME(""), CHILD(named, prof_stats_bins_i)}};

static const ctl_indexed_node_t prof_stats_bins_node[] = {
    {INDEX(prof_stats_bins_i)}};

static const ctl_named_node_t prof_stats_lextents_i_node[] = {
    {NAME("live"), CTL(prof_stats_lextents_i_live)},
    {NAME("accum"), CTL(prof_stats_lextents_i_accum)}};

static const ctl_named_node_t super_prof_stats_lextents_i_node[] = {
    {NAME(""), CHILD(named, prof_stats_lextents_i)}};

static const ctl_indexed_node_t prof_stats_lextents_node[] = {
    {INDEX(prof_stats_lextents_i)}};

static const ctl_named_node_t prof_stats_node[] = {
    {NAME("bins"), CHILD(indexed, prof_stats_bins)},
    {NAME("lextents"), CHILD(indexed, prof_stats_lextents)},
};

static const ctl_named_node_t prof_node[] = {
    {NAME("thread_active_init"), CTL(prof_thread_active_init)},
    {NAME("active"), CTL(prof_active)}, {NAME("dump"), CTL(prof_dump)},
    {NAME("gdump"), CTL(prof_gdump)}, {NAME("prefix"), CTL(prof_prefix)},
    {NAME("reset"), CTL(prof_reset)}, {NAME("interval"), CTL(prof_interval)},
    {NAME("lg_sample"), CTL(lg_prof_sample)},
    {NAME("log_start"), CTL(prof_log_start)},
    {NAME("log_stop"), CTL(prof_log_stop)},
    {NAME("stats"), CHILD(named, prof_stats)}};

static const ctl_named_node_t stats_arenas_i_small_node[] = {
    {NAME("allocated"), CTL(stats_arenas_i_small_allocated)},
    {NAME("nmalloc"), CTL(stats_arenas_i_small_nmalloc)},
    {NAME("ndalloc"), CTL(stats_arenas_i_small_ndalloc)},
    {NAME("nrequests"), CTL(stats_arenas_i_small_nrequests)},
    {NAME("nfills"), CTL(stats_arenas_i_small_nfills)},
    {NAME("nflushes"), CTL(stats_arenas_i_small_nflushes)}};

static const ctl_named_node_t stats_arenas_i_large_node[] = {
    {NAME("allocated"), CTL(stats_arenas_i_large_allocated)},
    {NAME("nmalloc"), CTL(stats_arenas_i_large_nmalloc)},
    {NAME("ndalloc"), CTL(stats_arenas_i_large_ndalloc)},
    {NAME("nrequests"), CTL(stats_arenas_i_large_nrequests)},
    {NAME("nfills"), CTL(stats_arenas_i_large_nfills)},
    {NAME("nflushes"), CTL(stats_arenas_i_large_nflushes)}};

#define MUTEX_PROF_DATA_NODE(prefix)                                                                          \
	static const ctl_named_node_t stats_##prefix##_node[] = {                                             \
	    {NAME("num_ops"), CTL(stats_##prefix##_num_ops)},                                                 \
	    {NAME("num_wait"), CTL(stats_##prefix##_num_wait)},                                               \
	    {NAME("num_spin_acq"), CTL(stats_##prefix##_num_spin_acq)},                                       \
	    {NAME("num_owner_switch"),                                                                        \
	        CTL(stats_##prefix##_num_owner_switch)},                                                      \
	    {NAME("total_wait_time"), CTL(stats_##prefix##_total_wait_time)},                                 \
	    {NAME("max_wait_time"), CTL(stats_##prefix##_max_wait_time)},                                     \
	    {NAME("max_num_thds"),                                                                            \
	        CTL(stats_##prefix##_max_num_thds)} /* Note that # of current waiting thread not provided. */ \
	};

MUTEX_PROF_DATA_NODE(arenas_i_bins_j_mutex)

static const ctl_named_node_t stats_arenas_i_bins_j_node[] = {
    {NAME("nmalloc"), CTL(stats_arenas_i_bins_j_nmalloc)},
    {NAME("ndalloc"), CTL(stats_arenas_i_bins_j_ndalloc)},
    {NAME("nrequests"), CTL(stats_arenas_i_bins_j_nrequests)},
    {NAME("curregs"), CTL(stats_arenas_i_bins_j_curregs)},
    {NAME("nfills"), CTL(stats_arenas_i_bins_j_nfills)},
    {NAME("nflushes"), CTL(stats_arenas_i_bins_j_nflushes)},
    {NAME("nslabs"), CTL(stats_arenas_i_bins_j_nslabs)},
    {NAME("nreslabs"), CTL(stats_arenas_i_bins_j_nreslabs)},
    {NAME("curslabs"), CTL(stats_arenas_i_bins_j_curslabs)},
    {NAME("nonfull_slabs"), CTL(stats_arenas_i_bins_j_nonfull_slabs)},
    {NAME("mutex"), CHILD(named, stats_arenas_i_bins_j_mutex)}};

static const ctl_named_node_t super_stats_arenas_i_bins_j_node[] = {
    {NAME(""), CHILD(named, stats_arenas_i_bins_j)}};

static const ctl_indexed_node_t stats_arenas_i_bins_node[] = {
    {INDEX(stats_arenas_i_bins_j)}};

static const ctl_named_node_t stats_arenas_i_lextents_j_node[] = {
    {NAME("nmalloc"), CTL(stats_arenas_i_lextents_j_nmalloc)},
    {NAME("ndalloc"), CTL(stats_arenas_i_lextents_j_ndalloc)},
    {NAME("nrequests"), CTL(stats_arenas_i_lextents_j_nrequests)},
    {NAME("curlextents"), CTL(stats_arenas_i_lextents_j_curlextents)}};
static const ctl_named_node_t super_stats_arenas_i_lextents_j_node[] = {
    {NAME(""), CHILD(named, stats_arenas_i_lextents_j)}};

static const ctl_indexed_node_t stats_arenas_i_lextents_node[] = {
    {INDEX(stats_arenas_i_lextents_j)}};

static const ctl_named_node_t stats_arenas_i_extents_j_node[] = {
    {NAME("ndirty"), CTL(stats_arenas_i_extents_j_ndirty)},
    {NAME("nmuzzy"), CTL(stats_arenas_i_extents_j_nmuzzy)},
    {NAME("nretained"), CTL(stats_arenas_i_extents_j_nretained)},
    {NAME("npinned"), CTL(stats_arenas_i_extents_j_npinned)},
    {NAME("dirty_bytes"), CTL(stats_arenas_i_extents_j_dirty_bytes)},
    {NAME("muzzy_bytes"), CTL(stats_arenas_i_extents_j_muzzy_bytes)},
    {NAME("retained_bytes"), CTL(stats_arenas_i_extents_j_retained_bytes)},
    {NAME("pinned_bytes"), CTL(stats_arenas_i_extents_j_pinned_bytes)}};

static const ctl_named_node_t super_stats_arenas_i_extents_j_node[] = {
    {NAME(""), CHILD(named, stats_arenas_i_extents_j)}};

static const ctl_indexed_node_t stats_arenas_i_extents_node[] = {
    {INDEX(stats_arenas_i_extents_j)}};

#define OP(mtx) MUTEX_PROF_DATA_NODE(arenas_i_mutexes_##mtx)
MUTEX_PROF_ARENA_MUTEXES
#undef OP

static const ctl_named_node_t stats_arenas_i_mutexes_node[] = {
#define OP(mtx) {NAME(#mtx), CHILD(named, stats_arenas_i_mutexes_##mtx)},
    MUTEX_PROF_ARENA_MUTEXES
#undef OP
};

static const ctl_named_node_t stats_arenas_i_hpa_shard_slabs_node[] = {
    {NAME("npageslabs_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_slabs_npageslabs_nonhuge)},
    {NAME("npageslabs_huge"),
        CTL(stats_arenas_i_hpa_shard_slabs_npageslabs_huge)},
    {NAME("nactive_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_slabs_nactive_nonhuge)},
    {NAME("nactive_huge"), CTL(stats_arenas_i_hpa_shard_slabs_nactive_huge)},
    {NAME("ndirty_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_slabs_ndirty_nonhuge)},
    {NAME("ndirty_huge"), CTL(stats_arenas_i_hpa_shard_slabs_ndirty_huge)}};

static const ctl_named_node_t stats_arenas_i_hpa_shard_full_slabs_node[] = {
    {NAME("npageslabs_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_npageslabs_nonhuge)},
    {NAME("npageslabs_huge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_npageslabs_huge)},
    {NAME("nactive_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_nactive_nonhuge)},
    {NAME("nactive_huge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_nactive_huge)},
    {NAME("ndirty_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_ndirty_nonhuge)},
    {NAME("ndirty_huge"),
        CTL(stats_arenas_i_hpa_shard_full_slabs_ndirty_huge)}};

static const ctl_named_node_t stats_arenas_i_hpa_shard_empty_slabs_node[] = {
    {NAME("npageslabs_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_nonhuge)},
    {NAME("npageslabs_huge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_npageslabs_huge)},
    {NAME("nactive_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_nactive_nonhuge)},
    {NAME("nactive_huge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_nactive_huge)},
    {NAME("ndirty_nonhuge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_ndirty_nonhuge)},
    {NAME("ndirty_huge"),
        CTL(stats_arenas_i_hpa_shard_empty_slabs_ndirty_huge)}};

static const ctl_named_node_t stats_arenas_i_hpa_shard_nonfull_slabs_j_node[] =
    {{NAME("npageslabs_nonhuge"),
         CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_nonhuge)},
        {NAME("npageslabs_huge"),
            CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_npageslabs_huge)},
        {NAME("nactive_nonhuge"),
            CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_nonhuge)},
        {NAME("nactive_huge"),
            CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_nactive_huge)},
        {NAME("ndirty_nonhuge"),
            CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_nonhuge)},
        {NAME("ndirty_huge"),
            CTL(stats_arenas_i_hpa_shard_nonfull_slabs_j_ndirty_huge)}};

static const ctl_named_node_t
    super_stats_arenas_i_hpa_shard_nonfull_slabs_j_node[] = {
        {NAME(""), CHILD(named, stats_arenas_i_hpa_shard_nonfull_slabs_j)}};

static const ctl_indexed_node_t stats_arenas_i_hpa_shard_nonfull_slabs_node[] =
    {{INDEX(stats_arenas_i_hpa_shard_nonfull_slabs_j)}};

static const ctl_named_node_t stats_arenas_i_hpa_shard_alloc_j_node[] = {
    {NAME("min_extents"), CTL(stats_arenas_i_hpa_shard_alloc_j_min_extents)},
    {NAME("max_extents"), CTL(stats_arenas_i_hpa_shard_alloc_j_max_extents)},
    {NAME("extents"), CTL(stats_arenas_i_hpa_shard_alloc_j_extents)},
    {NAME("ps"), CTL(stats_arenas_i_hpa_shard_alloc_j_ps)},
    {NAME("pages_per_ps"), CTL(stats_arenas_i_hpa_shard_alloc_j_pages_per_ps)},
    {NAME("extents_per_ps"),
        CTL(stats_arenas_i_hpa_shard_alloc_j_extents_per_ps)},
    {NAME("total_elapsed_ns_per_ps"),
        CTL(stats_arenas_i_hpa_shard_alloc_j_total_elapsed_ns_per_ps)}};

static const ctl_named_node_t super_stats_arenas_i_hpa_shard_alloc_j_node[] = {
    {NAME(""), CHILD(named, stats_arenas_i_hpa_shard_alloc_j)}};

static const ctl_indexed_node_t stats_arenas_i_hpa_shard_alloc_node[] = {
    {INDEX(stats_arenas_i_hpa_shard_alloc_j)}};

static const ctl_named_node_t stats_arenas_i_hpa_shard_node[] = {
    {NAME("npageslabs"), CTL(stats_arenas_i_hpa_shard_npageslabs)},
    {NAME("nactive"), CTL(stats_arenas_i_hpa_shard_nactive)},
    {NAME("ndirty"), CTL(stats_arenas_i_hpa_shard_ndirty)},

    {NAME("slabs"), CHILD(named, stats_arenas_i_hpa_shard_slabs)},

    {NAME("npurge_passes"), CTL(stats_arenas_i_hpa_shard_npurge_passes)},
    {NAME("npurges"), CTL(stats_arenas_i_hpa_shard_npurges)},
    {NAME("nhugifies"), CTL(stats_arenas_i_hpa_shard_nhugifies)},
    {NAME("nhugify_failures"), CTL(stats_arenas_i_hpa_shard_nhugify_failures)},
    {NAME("ndehugifies"), CTL(stats_arenas_i_hpa_shard_ndehugifies)},

    {NAME("alloc"), CHILD(indexed, stats_arenas_i_hpa_shard_alloc)},

    {NAME("full_slabs"), CHILD(named, stats_arenas_i_hpa_shard_full_slabs)},
    {NAME("empty_slabs"), CHILD(named, stats_arenas_i_hpa_shard_empty_slabs)},
    {NAME("nonfull_slabs"),
        CHILD(indexed, stats_arenas_i_hpa_shard_nonfull_slabs)}};

static const ctl_named_node_t stats_arenas_i_node[] = {
    {NAME("nthreads"), CTL(stats_arenas_i_nthreads)},
    {NAME("uptime"), CTL(stats_arenas_i_uptime)},
    {NAME("dss"), CTL(stats_arenas_i_dss)},
    {NAME("dirty_decay_ms"), CTL(stats_arenas_i_dirty_decay_ms)},
    {NAME("muzzy_decay_ms"), CTL(stats_arenas_i_muzzy_decay_ms)},
    {NAME("pactive"), CTL(stats_arenas_i_pactive)},
    {NAME("pdirty"), CTL(stats_arenas_i_pdirty)},
    {NAME("pmuzzy"), CTL(stats_arenas_i_pmuzzy)},
    {NAME("mapped"), CTL(stats_arenas_i_mapped)},
    {NAME("retained"), CTL(stats_arenas_i_retained)},
    {NAME("pinned"), CTL(stats_arenas_i_pinned)},
    {NAME("extent_avail"), CTL(stats_arenas_i_extent_avail)},
    {NAME("dirty_npurge"), CTL(stats_arenas_i_dirty_npurge)},
    {NAME("dirty_nmadvise"), CTL(stats_arenas_i_dirty_nmadvise)},
    {NAME("dirty_purged"), CTL(stats_arenas_i_dirty_purged)},
    {NAME("muzzy_npurge"), CTL(stats_arenas_i_muzzy_npurge)},
    {NAME("muzzy_nmadvise"), CTL(stats_arenas_i_muzzy_nmadvise)},
    {NAME("muzzy_purged"), CTL(stats_arenas_i_muzzy_purged)},
    {NAME("base"), CTL(stats_arenas_i_base)},
    {NAME("internal"), CTL(stats_arenas_i_internal)},
    {NAME("metadata_edata"), CTL(stats_arenas_i_metadata_edata)},
    {NAME("metadata_rtree"), CTL(stats_arenas_i_metadata_rtree)},
    {NAME("metadata_thp"), CTL(stats_arenas_i_metadata_thp)},
    {NAME("tcache_bytes"), CTL(stats_arenas_i_tcache_bytes)},
    {NAME("tcache_stashed_bytes"), CTL(stats_arenas_i_tcache_stashed_bytes)},
    {NAME("resident"), CTL(stats_arenas_i_resident)},
    {NAME("abandoned_vm"), CTL(stats_arenas_i_abandoned_vm)},
    {NAME("hpa_sec_bytes"), CTL(stats_arenas_i_hpa_sec_bytes)},
    {NAME("hpa_sec_hits"), CTL(stats_arenas_i_hpa_sec_hits)},
    {NAME("hpa_sec_misses"), CTL(stats_arenas_i_hpa_sec_misses)},
    {NAME("hpa_sec_dalloc_noflush"),
        CTL(stats_arenas_i_hpa_sec_dalloc_noflush)},
    {NAME("hpa_sec_dalloc_flush"), CTL(stats_arenas_i_hpa_sec_dalloc_flush)},
    {NAME("hpa_sec_overfills"), CTL(stats_arenas_i_hpa_sec_overfills)},
    {NAME("small"), CHILD(named, stats_arenas_i_small)},
    {NAME("large"), CHILD(named, stats_arenas_i_large)},
    {NAME("bins"), CHILD(indexed, stats_arenas_i_bins)},
    {NAME("lextents"), CHILD(indexed, stats_arenas_i_lextents)},
    {NAME("extents"), CHILD(indexed, stats_arenas_i_extents)},
    {NAME("mutexes"), CHILD(named, stats_arenas_i_mutexes)},
    {NAME("hpa_shard"), CHILD(named, stats_arenas_i_hpa_shard)}};
static const ctl_named_node_t super_stats_arenas_i_node[] = {
    {NAME(""), CHILD(named, stats_arenas_i)}};

static const ctl_indexed_node_t stats_arenas_node[] = {{INDEX(stats_arenas_i)}};

static const ctl_named_node_t stats_background_thread_node[] = {
    {NAME("num_threads"), CTL(stats_background_thread_num_threads)},
    {NAME("num_runs"), CTL(stats_background_thread_num_runs)},
    {NAME("run_interval"), CTL(stats_background_thread_run_interval)}};

#define OP(mtx) MUTEX_PROF_DATA_NODE(mutexes_##mtx)
MUTEX_PROF_GLOBAL_MUTEXES
#undef OP

static const ctl_named_node_t stats_mutexes_node[] = {
#define OP(mtx) {NAME(#mtx), CHILD(named, stats_mutexes_##mtx)},
    MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
    {NAME("reset"), CTL(stats_mutexes_reset)}};
#undef MUTEX_PROF_DATA_NODE

static const ctl_named_node_t approximate_stats_node[] = {
    {NAME("active"), CTL(approximate_stats_active)},
};

static const ctl_named_node_t stats_node[] = {
    {NAME("allocated"), CTL(stats_allocated)},
    {NAME("active"), CTL(stats_active)},
    {NAME("metadata"), CTL(stats_metadata)},
    {NAME("metadata_edata"), CTL(stats_metadata_edata)},
    {NAME("metadata_rtree"), CTL(stats_metadata_rtree)},
    {NAME("metadata_thp"), CTL(stats_metadata_thp)},
    {NAME("resident"), CTL(stats_resident)},
    {NAME("mapped"), CTL(stats_mapped)},
    {NAME("retained"), CTL(stats_retained)},
    {NAME("pinned"), CTL(stats_pinned)},
    {NAME("background_thread"), CHILD(named, stats_background_thread)},
    {NAME("mutexes"), CHILD(named, stats_mutexes)},
    {NAME("arenas"), CHILD(indexed, stats_arenas)},
    {NAME("zero_reallocs"), CTL(stats_zero_reallocs)},
};

static const ctl_named_node_t experimental_hooks_node[] = {
    {NAME("prof_backtrace"), CTL(experimental_hooks_prof_backtrace)},
    {NAME("prof_dump"), CTL(experimental_hooks_prof_dump)},
    {NAME("prof_sample"), CTL(experimental_hooks_prof_sample)},
    {NAME("prof_sample_free"), CTL(experimental_hooks_prof_sample_free)},
    {NAME("thread_event"), CTL(experimental_hooks_thread_event)},
};

static const ctl_named_node_t experimental_prof_recent_node[] = {
    {NAME("alloc_max"), CTL(experimental_prof_recent_alloc_max)},
    {NAME("alloc_dump"), CTL(experimental_prof_recent_alloc_dump)},
};

static const ctl_named_node_t experimental_utilization_node[] = {
    {NAME("batch_query"), CTL(experimental_utilization_batch_query)}};

static const ctl_named_node_t experimental_node[] = {
    {NAME("hooks"), CHILD(named, experimental_hooks)},
    {NAME("arenas_create_ext"), CTL(experimental_arenas_create_ext)},
    {NAME("prof_recent"), CHILD(named, experimental_prof_recent)},
    {NAME("utilization"), CHILD(named, experimental_utilization)}};

static const ctl_named_node_t root_node[] = {{NAME("version"), CTL(version)},
    {NAME("epoch"), CTL(epoch)},
    {NAME("background_thread"), CTL(background_thread)},
    {NAME("max_background_threads"), CTL(max_background_threads)},
    {NAME("thread"), CHILD(named, thread)},
    {NAME("config"), CHILD(named, config)}, {NAME("opt"), CHILD(named, opt)},
    {NAME("tcache"), CHILD(named, tcache)},
    {NAME("arena"), CHILD(indexed, arena)},
    {NAME("arenas"), CHILD(named, arenas)}, {NAME("prof"), CHILD(named, prof)},
    {NAME("stats"), CHILD(named, stats)},
    {NAME("approximate_stats"), CHILD(named, approximate_stats)},
    {NAME("experimental"), CHILD(named, experimental)}};
static const ctl_named_node_t super_root_node[] = {
    {NAME(""), CHILD(named, root)}};

#undef NAME
#undef CHILD
#undef CTL
#undef INDEX

/******************************************************************************/

static void
ctl_refresh(tsdn_t *tsdn) {
	malloc_mutex_assert_owner(tsdn, &ctl_mtx);
	ctl_arena_t *ctl_sarena = ctl_arenas_refresh(tsdn);
	ctl_stats_refresh(tsdn, ctl_sarena);
	ctl_arenas_epoch_advance();
}

static bool
ctl_init(tsd_t *tsd) {
	bool    ret;
	tsdn_t *tsdn = tsd_tsdn(tsd);

	malloc_mutex_lock(tsdn, &ctl_mtx);
	if (!ctl_initialized) {
		if (ctl_stats_init(tsdn)) {
			ret = true;
			goto label_return;
		}

		if (ctl_arenas_init(tsd)) {
			ret = true;
			goto label_return;
		}
		ctl_refresh(tsdn);

		ctl_initialized = true;
	}

	ret = false;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}

static int
ctl_lookup(tsdn_t *tsdn, const ctl_named_node_t *starting_node,
    const char *name, const ctl_named_node_t **ending_nodep, size_t *mibp,
    size_t *depthp) {
	int                     ret;
	const char             *elm, *tdot, *dot;
	size_t                  elen, i, j;
	const ctl_named_node_t *node;

	elm = name;
	/* Equivalent to strchrnul(). */
	dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot : strchr(elm, '\0');
	elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	if (elen == 0) {
		ret = ENOENT;
		goto label_return;
	}
	node = starting_node;
	for (i = 0; i < *depthp; i++) {
		assert(node);
		assert(node->nchildren > 0);
		if (ctl_named_node(node->children) != NULL) {
			const ctl_named_node_t *pnode = node;

			/* Children are named. */
			for (j = 0; j < node->nchildren; j++) {
				const ctl_named_node_t *child =
				    ctl_named_children(node, j);
				if (strlen(child->name) == elen
				    && strncmp(elm, child->name, elen) == 0) {
					node = child;
					mibp[i] = j;
					break;
				}
			}
			if (node == pnode) {
				ret = ENOENT;
				goto label_return;
			}
		} else {
			uintmax_t                 index;
			const ctl_indexed_node_t *inode;

			/* Children are indexed. */
			index = malloc_strtoumax(elm, NULL, 10);
			if (index == UINTMAX_MAX || index > SIZE_T_MAX) {
				ret = ENOENT;
				goto label_return;
			}

			inode = ctl_indexed_node(node->children);
			node = inode->index(tsdn, mibp, *depthp, (size_t)index);
			if (node == NULL) {
				ret = ENOENT;
				goto label_return;
			}

			mibp[i] = (size_t)index;
		}

		/* Reached the end? */
		if (node->ctl != NULL || *dot == '\0') {
			/* Terminal node. */
			if (*dot != '\0') {
				/*
				 * The name contains more elements than are
				 * in this path through the tree.
				 */
				ret = ENOENT;
				goto label_return;
			}
			/* Complete lookup successful. */
			*depthp = i + 1;
			break;
		}

		/* Update elm. */
		elm = &dot[1];
		dot = ((tdot = strchr(elm, '.')) != NULL) ? tdot
		                                          : strchr(elm, '\0');
		elen = (size_t)((uintptr_t)dot - (uintptr_t)elm);
	}
	if (ending_nodep != NULL) {
		*ending_nodep = node;
	}

	ret = 0;
label_return:
	return ret;
}

int
ctl_byname(tsd_t *tsd, const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen) {
	int                     ret;
	size_t                  depth;
	size_t                  mib[CTL_MAX_DEPTH];
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	depth = CTL_MAX_DEPTH;
	ret = ctl_lookup(
	    tsd_tsdn(tsd), super_root_node, name, &node, mib, &depth);
	if (ret != 0) {
		goto label_return;
	}

	if (node != NULL && node->ctl) {
		ret = node->ctl(tsd, mib, depth, oldp, oldlenp, newp, newlen);
	} else {
		/* The name refers to a partial path through the ctl tree. */
		ret = ENOENT;
	}

label_return:
	return (ret);
}

static const ctl_named_node_t *
arena_i_index(tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	return ctl_arena_i_indexable(tsdn, i) ? super_arena_i_node : NULL;
}

static const ctl_named_node_t *
arenas_bin_i_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	if (i >= SC_NBINS) {
		return NULL;
	}
	return super_arenas_bin_i_node;
}

static const ctl_named_node_t *
arenas_lextent_i_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	if (i >= SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_arenas_lextent_i_node;
}

int
ctl_nametomib(tsd_t *tsd, const char *name, size_t *mibp, size_t *miblenp) {
	int ret;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookup(
	    tsd_tsdn(tsd), super_root_node, name, NULL, mibp, miblenp);
label_return:
	return (ret);
}

static int
ctl_lookupbymib(tsdn_t *tsdn, const ctl_named_node_t **ending_nodep,
    const size_t *mib, size_t miblen) {
	int ret;

	const ctl_named_node_t *node = super_root_node;
	for (size_t i = 0; i < miblen; i++) {
		assert(node);
		assert(node->nchildren > 0);
		if (ctl_named_node(node->children) != NULL) {
			/* Children are named. */
			if (node->nchildren <= mib[i]) {
				ret = ENOENT;
				goto label_return;
			}
			node = ctl_named_children(node, mib[i]);
		} else {
			const ctl_indexed_node_t *inode;

			/* Indexed element. */
			inode = ctl_indexed_node(node->children);
			node = inode->index(tsdn, mib, miblen, mib[i]);
			if (node == NULL) {
				ret = ENOENT;
				goto label_return;
			}
		}
	}
	assert(ending_nodep != NULL);
	*ending_nodep = node;
	ret = 0;

label_return:
	return (ret);
}

int
ctl_bymib(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int                     ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}

	/* Call the ctl function. */
	if (node && node->ctl) {
		ret = node->ctl(tsd, mib, miblen, oldp, oldlenp, newp, newlen);
	} else {
		/* Partial MIB. */
		ret = ENOENT;
	}

label_return:
	return (ret);
}

int
ctl_mibnametomib(
    tsd_t *tsd, size_t *mib, size_t miblen, const char *name, size_t *miblenp) {
	int                     ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}
	if (node == NULL || node->ctl != NULL) {
		ret = ENOENT;
		goto label_return;
	}

	assert(miblenp != NULL);
	assert(*miblenp >= miblen);
	*miblenp -= miblen;
	ret = ctl_lookup(
	    tsd_tsdn(tsd), node, name, NULL, mib + miblen, miblenp);
	*miblenp += miblen;
label_return:
	return (ret);
}

int
ctl_bymibname(tsd_t *tsd, size_t *mib, size_t miblen, const char *name,
    size_t *miblenp, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int                     ret;
	const ctl_named_node_t *node;

	if (!ctl_initialized && ctl_init(tsd)) {
		ret = EAGAIN;
		goto label_return;
	}

	ret = ctl_lookupbymib(tsd_tsdn(tsd), &node, mib, miblen);
	if (ret != 0) {
		goto label_return;
	}
	if (node == NULL || node->ctl != NULL) {
		ret = ENOENT;
		goto label_return;
	}

	assert(miblenp != NULL);
	assert(*miblenp >= miblen);
	*miblenp -= miblen;
	/*
	 * The same node supplies the starting node and stores the ending node.
	 */
	ret = ctl_lookup(
	    tsd_tsdn(tsd), node, name, &node, mib + miblen, miblenp);
	*miblenp += miblen;
	if (ret != 0) {
		goto label_return;
	}

	if (node != NULL && node->ctl) {
		ret = node->ctl(
		    tsd, mib, *miblenp, oldp, oldlenp, newp, newlen);
	} else {
		/* The name refers to a partial path through the ctl tree. */
		ret = ENOENT;
	}

label_return:
	return (ret);
}

bool
ctl_boot(void) {
	if (malloc_mutex_init(&ctl_mtx, "ctl", WITNESS_RANK_CTL,
	        malloc_mutex_rank_exclusive)) {
		return true;
	}

	ctl_initialized = false;

	return false;
}

void
ctl_prefork(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &ctl_mtx);
}

void
ctl_postfork_parent(tsdn_t *tsdn) {
	malloc_mutex_postfork_parent(tsdn, &ctl_mtx);
}

void
ctl_postfork_child(tsdn_t *tsdn) {
	malloc_mutex_postfork_child(tsdn, &ctl_mtx);
}

void
ctl_mtx_lock(tsdn_t *tsdn) {
	malloc_mutex_lock(tsdn, &ctl_mtx);
}

void
ctl_mtx_unlock(tsdn_t *tsdn) {
	malloc_mutex_unlock(tsdn, &ctl_mtx);
}

void
ctl_mtx_assert_held(tsdn_t *tsdn) {
	malloc_mutex_assert_owner(tsdn, &ctl_mtx);
}

void
ctl_mtx_prof_read(tsdn_t *tsdn, mutex_prof_data_t *mutex_prof_data) {
	malloc_mutex_prof_read(tsdn, mutex_prof_data, &ctl_mtx);
}

void
ctl_mtx_prof_data_reset(tsdn_t *tsdn) {
	malloc_mutex_lock(tsdn, &ctl_mtx);
	malloc_mutex_prof_data_reset(tsdn, &ctl_mtx);
	malloc_mutex_unlock(tsdn, &ctl_mtx);
}

/******************************************************************************/

static int
version_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		const char *oldval = JEMALLOC_VERSION;
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(const char *));
	}
	return ret;
}

static int
epoch_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	UNUSED uint64_t newval;

	malloc_mutex_lock(tsd_tsdn(tsd), &ctl_mtx);
	int ret = ctl_write(&newval, sizeof(newval), newp, newlen);
	if (ret == 0) {
		if (newp != NULL) {
			ctl_refresh(tsd_tsdn(tsd));
		}
		uint64_t epoch = ctl_arenas_epoch_get();
		ret = ctl_read(oldp, oldlenp, &epoch, sizeof(epoch));
	}

	malloc_mutex_unlock(tsd_tsdn(tsd), &ctl_mtx);
	return ret;
}

/******************************************************************************/

static const ctl_named_node_t *
stats_arenas_i_bins_j_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t j) {
	if (j >= SC_NBINS) {
		return NULL;
	}
	return super_stats_arenas_i_bins_j_node;
}


static const ctl_named_node_t *
stats_arenas_i_lextents_j_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t j) {
	if (j >= SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_stats_arenas_i_lextents_j_node;
}


static const ctl_named_node_t *
stats_arenas_i_extents_j_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t j) {
	if (j >= SC_NPSIZES) {
		return NULL;
	}
	return super_stats_arenas_i_extents_j_node;
}


static const ctl_named_node_t *
stats_arenas_i_hpa_shard_nonfull_slabs_j_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t j) {
	if (j >= PSSET_NPSIZES) {
		return NULL;
	}
	return super_stats_arenas_i_hpa_shard_nonfull_slabs_j_node;
}


static const ctl_named_node_t *
stats_arenas_i_hpa_shard_alloc_j_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t j) {
	if (j > SEC_MAX_NALLOCS) {
		return NULL;
	}
	return super_stats_arenas_i_hpa_shard_alloc_j_node;
}


static const ctl_named_node_t *
stats_arenas_i_index(tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	const ctl_named_node_t *ret;

	malloc_mutex_lock(tsdn, &ctl_mtx);
	if (ctl_arenas_i_verify(i, ctl_narenas_get(tsdn))) {
		ret = NULL;
		goto label_return;
	}

	ret = super_stats_arenas_i_node;
label_return:
	malloc_mutex_unlock(tsdn, &ctl_mtx);
	return ret;
}


static const ctl_named_node_t *
prof_stats_bins_i_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return NULL;
	}
	if (i >= SC_NBINS) {
		return NULL;
	}
	return super_prof_stats_bins_i_node;
}

static const ctl_named_node_t *
prof_stats_lextents_i_index(
    tsdn_t *tsdn, const size_t *mib, size_t miblen, size_t i) {
	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return NULL;
	}
	if (i >= SC_NSIZES - SC_NBINS) {
		return NULL;
	}
	return super_prof_stats_lextents_i_node;
}
