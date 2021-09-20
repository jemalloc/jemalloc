#define opt_abort JEMALLOC_N(opt_abort)
#define opt_abort_conf JEMALLOC_N(opt_abort_conf)
#define opt_confirm_conf JEMALLOC_N(opt_confirm_conf)
#define opt_hpa JEMALLOC_N(opt_hpa)
#define opt_junk_alloc JEMALLOC_N(opt_junk_alloc)
#define opt_junk_free JEMALLOC_N(opt_junk_free)
#define opt_utrace JEMALLOC_N(opt_utrace)
#define opt_xmalloc JEMALLOC_N(opt_xmalloc)
#define opt_experimental_infallible_new JEMALLOC_N(opt_experimental_infallible_new)
#define opt_zero JEMALLOC_N(opt_zero)
#define opt_narenas JEMALLOC_N(opt_narenas)
#define opt_zero_realloc_action JEMALLOC_N(opt_zero_realloc_action)
#define zero_realloc_count JEMALLOC_N(zero_realloc_count)
#define arenas JEMALLOC_N(arenas)
#define jet_malloc JEMALLOC_N(jet_malloc)
#define jet_calloc JEMALLOC_N(jet_calloc)
#define jet_posix_memalign JEMALLOC_N(jet_posix_memalign)
#define jet_aligned_alloc JEMALLOC_N(jet_aligned_alloc)
#define jet_realloc JEMALLOC_N(jet_realloc)
#define jet_free JEMALLOC_N(jet_free)
#define jet_mallocx JEMALLOC_N(jet_mallocx)
#define jet_rallocx JEMALLOC_N(jet_rallocx)
#define jet_xallocx JEMALLOC_N(jet_xallocx)
#define jet_sallocx JEMALLOC_N(jet_sallocx)
#define jet_dallocx JEMALLOC_N(jet_dallocx)
#define jet_sdallocx JEMALLOC_N(jet_sdallocx)
#define jet_nallocx JEMALLOC_N(jet_nallocx)
#define jet_mallctl JEMALLOC_N(jet_mallctl)
#define jet_mallctlnametomib JEMALLOC_N(jet_mallctlnametomib)
#define jet_mallctlbymib JEMALLOC_N(jet_mallctlbymib)
#define jet_malloc_stats_print JEMALLOC_N(jet_malloc_stats_print)
#define jet_malloc_usable_size JEMALLOC_N(jet_malloc_usable_size)
#define a0malloc JEMALLOC_N(a0malloc)
#define a0dalloc JEMALLOC_N(a0dalloc)
#define bootstrap_malloc JEMALLOC_N(bootstrap_malloc)
#define bootstrap_calloc JEMALLOC_N(bootstrap_calloc)
#define bootstrap_free JEMALLOC_N(bootstrap_free)
#define arena_set JEMALLOC_N(arena_set)
#define narenas_total_get JEMALLOC_N(narenas_total_get)
#define arena_init JEMALLOC_N(arena_init)
#define arena_choose_hard JEMALLOC_N(arena_choose_hard)
#define arena_migrate JEMALLOC_N(arena_migrate)
#define arena_cleanup JEMALLOC_N(arena_cleanup)
#define batch_alloc JEMALLOC_N(batch_alloc)
#define jemalloc_prefork JEMALLOC_N(jemalloc_prefork)
#define jemalloc_postfork_parent JEMALLOC_N(jemalloc_postfork_parent)
#define jemalloc_postfork_child JEMALLOC_N(jemalloc_postfork_child)
#define je_sdallocx_noflags JEMALLOC_N(je_sdallocx_noflags)
#define malloc_default JEMALLOC_N(malloc_default)
#define sdallocx_default JEMALLOC_N(sdallocx_default)
#define h_steps JEMALLOC_N(h_steps)
#define arena_basic_stats_merge JEMALLOC_N(arena_basic_stats_merge)
#define arena_handle_new_dirty_pages JEMALLOC_N(arena_handle_new_dirty_pages)
#define arena_extent_alloc_large JEMALLOC_N(arena_extent_alloc_large)
#define arena_extent_dalloc_large_prep JEMALLOC_N(arena_extent_dalloc_large_prep)
#define arena_extent_ralloc_large_shrink JEMALLOC_N(arena_extent_ralloc_large_shrink)
#define arena_extent_ralloc_large_expand JEMALLOC_N(arena_extent_ralloc_large_expand)
#define arena_decay_ms_set JEMALLOC_N(arena_decay_ms_set)
#define arena_decay_ms_get JEMALLOC_N(arena_decay_ms_get)
#define arena_decay JEMALLOC_N(arena_decay)
#define arena_do_deferred_work JEMALLOC_N(arena_do_deferred_work)
#define arena_destroy JEMALLOC_N(arena_destroy)
#define arena_cache_bin_fill_small JEMALLOC_N(arena_cache_bin_fill_small)
#define arena_malloc_hard JEMALLOC_N(arena_malloc_hard)
#define arena_dalloc_promoted JEMALLOC_N(arena_dalloc_promoted)
#define arena_dalloc_bin_locked_handle_newly_empty JEMALLOC_N(arena_dalloc_bin_locked_handle_newly_empty)
#define arena_dalloc_bin_locked_handle_newly_nonempty JEMALLOC_N(arena_dalloc_bin_locked_handle_newly_nonempty)
#define arena_dalloc_small JEMALLOC_N(arena_dalloc_small)
#define arena_dss_prec_get JEMALLOC_N(arena_dss_prec_get)
#define arena_get_ehooks JEMALLOC_N(arena_get_ehooks)
#define arena_dss_prec_set JEMALLOC_N(arena_dss_prec_set)
#define arena_dirty_decay_ms_default_get JEMALLOC_N(arena_dirty_decay_ms_default_get)
#define arena_dirty_decay_ms_default_set JEMALLOC_N(arena_dirty_decay_ms_default_set)
#define arena_muzzy_decay_ms_default_get JEMALLOC_N(arena_muzzy_decay_ms_default_get)
#define arena_muzzy_decay_ms_default_set JEMALLOC_N(arena_muzzy_decay_ms_default_set)
#define arena_init_huge JEMALLOC_N(arena_init_huge)
#define arena_is_huge JEMALLOC_N(arena_is_huge)
#define arena_choose_huge JEMALLOC_N(arena_choose_huge)
#define arena_bin_choose JEMALLOC_N(arena_bin_choose)
#define arena_fill_small_fresh JEMALLOC_N(arena_fill_small_fresh)
#define arena_boot JEMALLOC_N(arena_boot)
#define opt_background_thread JEMALLOC_N(opt_background_thread)
#define opt_background_thread_hpa_interval_max_ms JEMALLOC_N(opt_background_thread_hpa_interval_max_ms)
#define opt_max_background_threads JEMALLOC_N(opt_max_background_threads)
#define background_threads_enable JEMALLOC_N(background_threads_enable)
#define background_threads_disable JEMALLOC_N(background_threads_disable)
#define background_thread_interval_check JEMALLOC_N(background_thread_interval_check)
#define background_thread_prefork0 JEMALLOC_N(background_thread_prefork0)
#define background_thread_prefork1 JEMALLOC_N(background_thread_prefork1)
#define background_thread_postfork_parent JEMALLOC_N(background_thread_postfork_parent)
#define background_thread_postfork_child JEMALLOC_N(background_thread_postfork_child)
#define background_thread_stats_read JEMALLOC_N(background_thread_stats_read)
#define background_thread_ctl_init JEMALLOC_N(background_thread_ctl_init)
#define b0get JEMALLOC_N(b0get)
#define base_new JEMALLOC_N(base_new)
#define base_delete JEMALLOC_N(base_delete)
#define base_ehooks_get JEMALLOC_N(base_ehooks_get)
#define base_extent_hooks_set JEMALLOC_N(base_extent_hooks_set)
#define base_alloc JEMALLOC_N(base_alloc)
#define base_alloc_edata JEMALLOC_N(base_alloc_edata)
#define base_stats_get JEMALLOC_N(base_stats_get)
#define base_prefork JEMALLOC_N(base_prefork)
#define base_postfork_parent JEMALLOC_N(base_postfork_parent)
#define base_postfork_child JEMALLOC_N(base_postfork_child)
#define base_boot JEMALLOC_N(base_boot)
#define bin_shard_sizes_boot JEMALLOC_N(bin_shard_sizes_boot)
#define bin_update_shard_size JEMALLOC_N(bin_update_shard_size)
#define bin_init JEMALLOC_N(bin_init)
#define __xmm@00000001000000010000000100000001 JEMALLOC_N(__xmm@00000001000000010000000100000001)
#define bin_info_boot JEMALLOC_N(bin_info_boot)
#define bitmap_info_init JEMALLOC_N(bitmap_info_init)
#define buf_writer_init JEMALLOC_N(buf_writer_init)
#define buf_writer_flush JEMALLOC_N(buf_writer_flush)
#define buf_writer_cb JEMALLOC_N(buf_writer_cb)
#define buf_writer_terminate JEMALLOC_N(buf_writer_terminate)
#define buf_writer_pipe JEMALLOC_N(buf_writer_pipe)
#define cache_bin_info_init JEMALLOC_N(cache_bin_info_init)
#define cache_bin_preincrement JEMALLOC_N(cache_bin_preincrement)
#define cache_bin_postincrement JEMALLOC_N(cache_bin_postincrement)
#define cache_bin_init JEMALLOC_N(cache_bin_init)
#define cache_bin_still_zero_initialized JEMALLOC_N(cache_bin_still_zero_initialized)
#define ckh_new JEMALLOC_N(ckh_new)
#define ckh_delete JEMALLOC_N(ckh_delete)
#define ckh_count JEMALLOC_N(ckh_count)
#define ckh_iter JEMALLOC_N(ckh_iter)
#define ckh_insert JEMALLOC_N(ckh_insert)
#define ckh_remove JEMALLOC_N(ckh_remove)
#define ckh_search JEMALLOC_N(ckh_search)
#define ckh_string_hash JEMALLOC_N(ckh_string_hash)
#define ckh_string_keycomp JEMALLOC_N(ckh_string_keycomp)
#define ckh_pointer_hash JEMALLOC_N(ckh_pointer_hash)
#define ckh_pointer_keycomp JEMALLOC_N(ckh_pointer_keycomp)
#define counter_prefork JEMALLOC_N(counter_prefork)
#define counter_postfork_parent JEMALLOC_N(counter_postfork_parent)
#define ctl_byname JEMALLOC_N(ctl_byname)
#define ctl_nametomib JEMALLOC_N(ctl_nametomib)
#define ctl_bymib JEMALLOC_N(ctl_bymib)
#define ctl_mibnametomib JEMALLOC_N(ctl_mibnametomib)
#define ctl_bymibname JEMALLOC_N(ctl_bymibname)
#define ctl_boot JEMALLOC_N(ctl_boot)
#define ctl_prefork JEMALLOC_N(ctl_prefork)
#define ctl_postfork_parent JEMALLOC_N(ctl_postfork_parent)
#define ctl_postfork_child JEMALLOC_N(ctl_postfork_child)
#define ctl_mtx_assert_held JEMALLOC_N(ctl_mtx_assert_held)
#define decay_ms_valid JEMALLOC_N(decay_ms_valid)
#define decay_init JEMALLOC_N(decay_init)
#define decay_reinit JEMALLOC_N(decay_reinit)
#define decay_maybe_advance_epoch JEMALLOC_N(decay_maybe_advance_epoch)
#define decay_ns_until_purge JEMALLOC_N(decay_ns_until_purge)
#define decay_deadline_init JEMALLOC_N(decay_deadline_init)
#define div_init JEMALLOC_N(div_init)
#define ecache_init JEMALLOC_N(ecache_init)
#define edata_avail_new JEMALLOC_N(edata_avail_new)
#define edata_avail_insert JEMALLOC_N(edata_avail_insert)
#define edata_avail_remove_first JEMALLOC_N(edata_avail_remove_first)
#define edata_avail_remove JEMALLOC_N(edata_avail_remove)
#define edata_avail_remove_any JEMALLOC_N(edata_avail_remove_any)
#define edata_heap_new JEMALLOC_N(edata_heap_new)
#define edata_heap_empty JEMALLOC_N(edata_heap_empty)
#define edata_heap_first JEMALLOC_N(edata_heap_first)
#define edata_heap_any JEMALLOC_N(edata_heap_any)
#define edata_heap_insert JEMALLOC_N(edata_heap_insert)
#define edata_heap_remove_first JEMALLOC_N(edata_heap_remove_first)
#define edata_heap_remove JEMALLOC_N(edata_heap_remove)
#define edata_heap_remove_any JEMALLOC_N(edata_heap_remove_any)
#define edata_cache_init JEMALLOC_N(edata_cache_init)
#define edata_cache_get JEMALLOC_N(edata_cache_get)
#define edata_cache_put JEMALLOC_N(edata_cache_put)
#define edata_cache_prefork JEMALLOC_N(edata_cache_prefork)
#define edata_cache_postfork_parent JEMALLOC_N(edata_cache_postfork_parent)
#define edata_cache_postfork_child JEMALLOC_N(edata_cache_postfork_child)
#define edata_cache_fast_init JEMALLOC_N(edata_cache_fast_init)
#define edata_cache_fast_get JEMALLOC_N(edata_cache_fast_get)
#define edata_cache_fast_put JEMALLOC_N(edata_cache_fast_put)
#define edata_cache_fast_disable JEMALLOC_N(edata_cache_fast_disable)
#define ehooks_default_alloc_impl JEMALLOC_N(ehooks_default_alloc_impl)
#define ehooks_default_dalloc_impl JEMALLOC_N(ehooks_default_dalloc_impl)
#define ehooks_default_destroy_impl JEMALLOC_N(ehooks_default_destroy_impl)
#define ehooks_default_commit_impl JEMALLOC_N(ehooks_default_commit_impl)
#define ehooks_default_decommit_impl JEMALLOC_N(ehooks_default_decommit_impl)
#define ehooks_default_purge_lazy_impl JEMALLOC_N(ehooks_default_purge_lazy_impl)
#define ehooks_default_split_impl JEMALLOC_N(ehooks_default_split_impl)
#define ehooks_default_merge_impl JEMALLOC_N(ehooks_default_merge_impl)
#define ehooks_default_zero_impl JEMALLOC_N(ehooks_default_zero_impl)
#define ehooks_init JEMALLOC_N(ehooks_init)
#define emap_init JEMALLOC_N(emap_init)
#define emap_remap JEMALLOC_N(emap_remap)
#define emap_update_edata_state JEMALLOC_N(emap_update_edata_state)
#define emap_try_acquire_edata_neighbor JEMALLOC_N(emap_try_acquire_edata_neighbor)
#define emap_try_acquire_edata_neighbor_expand JEMALLOC_N(emap_try_acquire_edata_neighbor_expand)
#define emap_release_edata JEMALLOC_N(emap_release_edata)
#define emap_register_boundary JEMALLOC_N(emap_register_boundary)
#define emap_register_interior JEMALLOC_N(emap_register_interior)
#define emap_deregister_boundary JEMALLOC_N(emap_deregister_boundary)
#define emap_deregister_interior JEMALLOC_N(emap_deregister_interior)
#define emap_split_prepare JEMALLOC_N(emap_split_prepare)
#define emap_split_commit JEMALLOC_N(emap_split_commit)
#define emap_merge_prepare JEMALLOC_N(emap_merge_prepare)
#define emap_merge_commit JEMALLOC_N(emap_merge_commit)
#define emap_do_assert_mapped JEMALLOC_N(emap_do_assert_mapped)
#define emap_do_assert_not_mapped JEMALLOC_N(emap_do_assert_not_mapped)
#define eset_init JEMALLOC_N(eset_init)
#define eset_npages_get JEMALLOC_N(eset_npages_get)
#define eset_nextents_get JEMALLOC_N(eset_nextents_get)
#define eset_nbytes_get JEMALLOC_N(eset_nbytes_get)
#define eset_insert JEMALLOC_N(eset_insert)
#define eset_remove JEMALLOC_N(eset_remove)
#define eset_fit JEMALLOC_N(eset_fit)
#define exp_grow_init JEMALLOC_N(exp_grow_init)
#define opt_lg_extent_max_active_fit JEMALLOC_N(opt_lg_extent_max_active_fit)
#define ecache_alloc JEMALLOC_N(ecache_alloc)
#define ecache_alloc_grow JEMALLOC_N(ecache_alloc_grow)
#define ecache_dalloc JEMALLOC_N(ecache_dalloc)
#define ecache_evict JEMALLOC_N(ecache_evict)
#define extent_dalloc_gap JEMALLOC_N(extent_dalloc_gap)
#define extent_dalloc_wrapper JEMALLOC_N(extent_dalloc_wrapper)
#define extent_commit_wrapper JEMALLOC_N(extent_commit_wrapper)
#define extent_boot JEMALLOC_N(extent_boot)
#define extent_dss_prec_get JEMALLOC_N(extent_dss_prec_get)
#define extent_dss_prec_set JEMALLOC_N(extent_dss_prec_set)
#define extent_alloc_dss JEMALLOC_N(extent_alloc_dss)
#define extent_in_dss JEMALLOC_N(extent_in_dss)
#define extent_dss_mergeable JEMALLOC_N(extent_dss_mergeable)
#define extent_dss_boot JEMALLOC_N(extent_dss_boot)
#define opt_retain JEMALLOC_N(opt_retain)
#define extent_alloc_mmap JEMALLOC_N(extent_alloc_mmap)
#define hook_boot JEMALLOC_N(hook_boot)
#define hook_install JEMALLOC_N(hook_install)
#define hook_remove JEMALLOC_N(hook_remove)
#define hook_invoke_alloc JEMALLOC_N(hook_invoke_alloc)
#define hook_invoke_dalloc JEMALLOC_N(hook_invoke_dalloc)
#define hook_invoke_expand JEMALLOC_N(hook_invoke_expand)
#define hpa_supported JEMALLOC_N(hpa_supported)
#define hpa_central_init JEMALLOC_N(hpa_central_init)
#define hpa_shard_init JEMALLOC_N(hpa_shard_init)
#define hpa_shard_stats_accum JEMALLOC_N(hpa_shard_stats_accum)
#define hpa_shard_stats_merge JEMALLOC_N(hpa_shard_stats_merge)
#define hpa_shard_disable JEMALLOC_N(hpa_shard_disable)
#define hpa_shard_destroy JEMALLOC_N(hpa_shard_destroy)
#define hpa_shard_set_deferral_allowed JEMALLOC_N(hpa_shard_set_deferral_allowed)
#define hpa_shard_do_deferred_work JEMALLOC_N(hpa_shard_do_deferred_work)
#define hpa_shard_prefork3 JEMALLOC_N(hpa_shard_prefork3)
#define hpa_shard_prefork4 JEMALLOC_N(hpa_shard_prefork4)
#define hpa_shard_postfork_parent JEMALLOC_N(hpa_shard_postfork_parent)
#define hpa_shard_postfork_child JEMALLOC_N(hpa_shard_postfork_child)
#define hpa_central_extract JEMALLOC_N(hpa_central_extract)
#define hpdata_age_heap_new JEMALLOC_N(hpdata_age_heap_new)
#define hpdata_age_heap_empty JEMALLOC_N(hpdata_age_heap_empty)
#define hpdata_age_heap_first JEMALLOC_N(hpdata_age_heap_first)
#define hpdata_age_heap_any JEMALLOC_N(hpdata_age_heap_any)
#define hpdata_age_heap_insert JEMALLOC_N(hpdata_age_heap_insert)
#define hpdata_age_heap_remove_first JEMALLOC_N(hpdata_age_heap_remove_first)
#define hpdata_age_heap_remove JEMALLOC_N(hpdata_age_heap_remove)
#define hpdata_age_heap_remove_any JEMALLOC_N(hpdata_age_heap_remove_any)
#define hpdata_init JEMALLOC_N(hpdata_init)
#define hpdata_reserve_alloc JEMALLOC_N(hpdata_reserve_alloc)
#define hpdata_unreserve JEMALLOC_N(hpdata_unreserve)
#define hpdata_purge_begin JEMALLOC_N(hpdata_purge_begin)
#define hpdata_purge_next JEMALLOC_N(hpdata_purge_next)
#define hpdata_purge_end JEMALLOC_N(hpdata_purge_end)
#define hpdata_hugify JEMALLOC_N(hpdata_hugify)
#define hpdata_dehugify JEMALLOC_N(hpdata_dehugify)
#define __xmm@ffffffffffffffffffffffffffffffff JEMALLOC_N(__xmm@ffffffffffffffffffffffffffffffff)
#define inspect_extent_util_stats_get JEMALLOC_N(inspect_extent_util_stats_get)
#define inspect_extent_util_stats_verbose_get JEMALLOC_N(inspect_extent_util_stats_verbose_get)
#define large_malloc JEMALLOC_N(large_malloc)
#define large_palloc JEMALLOC_N(large_palloc)
#define large_ralloc_no_move JEMALLOC_N(large_ralloc_no_move)
#define large_ralloc JEMALLOC_N(large_ralloc)
#define large_dalloc_prep_locked JEMALLOC_N(large_dalloc_prep_locked)
#define large_dalloc_finish JEMALLOC_N(large_dalloc_finish)
#define large_dalloc JEMALLOC_N(large_dalloc)
#define large_salloc JEMALLOC_N(large_salloc)
#define large_prof_info_get JEMALLOC_N(large_prof_info_get)
#define large_prof_tctx_reset JEMALLOC_N(large_prof_tctx_reset)
#define large_prof_info_set JEMALLOC_N(large_prof_info_set)
#define log_init_done JEMALLOC_N(log_init_done)
#define log_var_update_state JEMALLOC_N(log_var_update_state)
#define wrtmessage JEMALLOC_N(wrtmessage)
#define buferror JEMALLOC_N(buferror)
#define malloc_strtoumax JEMALLOC_N(malloc_strtoumax)
#define malloc_write JEMALLOC_N(malloc_write)
#define malloc_vsnprintf JEMALLOC_N(malloc_vsnprintf)
#define malloc_snprintf JEMALLOC_N(malloc_snprintf)
#define malloc_vcprintf JEMALLOC_N(malloc_vcprintf)
#define malloc_printf JEMALLOC_N(malloc_printf)
#define opt_mutex_max_spin JEMALLOC_N(opt_mutex_max_spin)
#define malloc_mutex_init JEMALLOC_N(malloc_mutex_init)
#define malloc_mutex_prefork JEMALLOC_N(malloc_mutex_prefork)
#define malloc_mutex_postfork_parent JEMALLOC_N(malloc_mutex_postfork_parent)
#define malloc_mutex_postfork_child JEMALLOC_N(malloc_mutex_postfork_child)
#define malloc_mutex_boot JEMALLOC_N(malloc_mutex_boot)
#define malloc_mutex_prof_data_reset JEMALLOC_N(malloc_mutex_prof_data_reset)
#define malloc_mutex_lock_slow JEMALLOC_N(malloc_mutex_lock_slow)
#define nstime_init JEMALLOC_N(nstime_init)
#define nstime_init2 JEMALLOC_N(nstime_init2)
#define nstime_ns JEMALLOC_N(nstime_ns)
#define nstime_sec JEMALLOC_N(nstime_sec)
#define nstime_msec JEMALLOC_N(nstime_msec)
#define nstime_nsec JEMALLOC_N(nstime_nsec)
#define nstime_copy JEMALLOC_N(nstime_copy)
#define nstime_compare JEMALLOC_N(nstime_compare)
#define nstime_iadd JEMALLOC_N(nstime_iadd)
#define nstime_subtract JEMALLOC_N(nstime_subtract)
#define nstime_isubtract JEMALLOC_N(nstime_isubtract)
#define nstime_imultiply JEMALLOC_N(nstime_imultiply)
#define nstime_idivide JEMALLOC_N(nstime_idivide)
#define nstime_divide JEMALLOC_N(nstime_divide)
#define nstime_init_update JEMALLOC_N(nstime_init_update)
#define nstime_prof_init_update JEMALLOC_N(nstime_prof_init_update)
#define pa_central_init JEMALLOC_N(pa_central_init)
#define pa_shard_init JEMALLOC_N(pa_shard_init)
#define pa_shard_enable_hpa JEMALLOC_N(pa_shard_enable_hpa)
#define pa_shard_disable_hpa JEMALLOC_N(pa_shard_disable_hpa)
#define pa_shard_reset JEMALLOC_N(pa_shard_reset)
#define pa_shard_destroy JEMALLOC_N(pa_shard_destroy)
#define pa_alloc JEMALLOC_N(pa_alloc)
#define pa_expand JEMALLOC_N(pa_expand)
#define pa_shrink JEMALLOC_N(pa_shrink)
#define pa_dalloc JEMALLOC_N(pa_dalloc)
#define pa_decay_ms_set JEMALLOC_N(pa_decay_ms_set)
#define pa_decay_ms_get JEMALLOC_N(pa_decay_ms_get)
#define pa_shard_set_deferral_allowed JEMALLOC_N(pa_shard_set_deferral_allowed)
#define pa_shard_do_deferred_work JEMALLOC_N(pa_shard_do_deferred_work)
#define pa_shard_retain_grow_limit_get_set JEMALLOC_N(pa_shard_retain_grow_limit_get_set)
#define pa_shard_prefork0 JEMALLOC_N(pa_shard_prefork0)
#define pa_shard_prefork2 JEMALLOC_N(pa_shard_prefork2)
#define pa_shard_prefork3 JEMALLOC_N(pa_shard_prefork3)
#define pa_shard_prefork4 JEMALLOC_N(pa_shard_prefork4)
#define pa_shard_prefork5 JEMALLOC_N(pa_shard_prefork5)
#define pa_shard_postfork_parent JEMALLOC_N(pa_shard_postfork_parent)
#define pa_shard_postfork_child JEMALLOC_N(pa_shard_postfork_child)
#define pa_shard_basic_stats_merge JEMALLOC_N(pa_shard_basic_stats_merge)
#define pa_shard_stats_merge JEMALLOC_N(pa_shard_stats_merge)
#define pa_shard_mtx_stats_read JEMALLOC_N(pa_shard_mtx_stats_read)
#define pai_alloc_batch_default JEMALLOC_N(pai_alloc_batch_default)
#define pai_dalloc_batch_default JEMALLOC_N(pai_dalloc_batch_default)
#define pac_init JEMALLOC_N(pac_init)
#define pac_decay_all JEMALLOC_N(pac_decay_all)
#define pac_maybe_decay_purge JEMALLOC_N(pac_maybe_decay_purge)
#define pac_retain_grow_limit_get_set JEMALLOC_N(pac_retain_grow_limit_get_set)
#define pac_decay_ms_set JEMALLOC_N(pac_decay_ms_set)
#define pac_decay_ms_get JEMALLOC_N(pac_decay_ms_get)
#define pac_reset JEMALLOC_N(pac_reset)
#define pac_destroy JEMALLOC_N(pac_destroy)
#define pages_map JEMALLOC_N(pages_map)
#define pages_unmap JEMALLOC_N(pages_unmap)
#define pages_commit JEMALLOC_N(pages_commit)
#define pages_decommit JEMALLOC_N(pages_decommit)
#define pages_purge_lazy JEMALLOC_N(pages_purge_lazy)
#define pages_purge_forced JEMALLOC_N(pages_purge_forced)
#define pages_huge JEMALLOC_N(pages_huge)
#define pages_nohuge JEMALLOC_N(pages_nohuge)
#define pages_dontdump JEMALLOC_N(pages_dontdump)
#define pages_dodump JEMALLOC_N(pages_dodump)
#define pages_boot JEMALLOC_N(pages_boot)
#define pages_set_thp_state JEMALLOC_N(pages_set_thp_state)
#define peak_event_update JEMALLOC_N(peak_event_update)
#define peak_event_zero JEMALLOC_N(peak_event_zero)
#define peak_event_max JEMALLOC_N(peak_event_max)
#define peak_alloc_event_handler JEMALLOC_N(peak_alloc_event_handler)
#define peak_dalloc_new_event_wait JEMALLOC_N(peak_dalloc_new_event_wait)
#define peak_dalloc_postponed_event_wait JEMALLOC_N(peak_dalloc_postponed_event_wait)
#define opt_prof JEMALLOC_N(opt_prof)
#define opt_prof_active JEMALLOC_N(opt_prof_active)
#define opt_prof_thread_active_init JEMALLOC_N(opt_prof_thread_active_init)
#define opt_lg_prof_sample JEMALLOC_N(opt_lg_prof_sample)
#define opt_lg_prof_interval JEMALLOC_N(opt_lg_prof_interval)
#define opt_prof_gdump JEMALLOC_N(opt_prof_gdump)
#define opt_prof_final JEMALLOC_N(opt_prof_final)
#define opt_prof_leak JEMALLOC_N(opt_prof_leak)
#define opt_prof_accum JEMALLOC_N(opt_prof_accum)
#define opt_prof_unbias JEMALLOC_N(opt_prof_unbias)
#define opt_prof_sys_thread_name JEMALLOC_N(opt_prof_sys_thread_name)
#define prof_interval JEMALLOC_N(prof_interval)
#define prof_booted JEMALLOC_N(prof_booted)
#define prof_tdata_init JEMALLOC_N(prof_tdata_init)
#define prof_tdata_reinit JEMALLOC_N(prof_tdata_reinit)
#define prof_alloc_rollback JEMALLOC_N(prof_alloc_rollback)
#define prof_malloc_sample_object JEMALLOC_N(prof_malloc_sample_object)
#define prof_free_sampled_object JEMALLOC_N(prof_free_sampled_object)
#define prof_tctx_create JEMALLOC_N(prof_tctx_create)
#define prof_idump JEMALLOC_N(prof_idump)
#define prof_mdump JEMALLOC_N(prof_mdump)
#define prof_gdump JEMALLOC_N(prof_gdump)
#define prof_tdata_cleanup JEMALLOC_N(prof_tdata_cleanup)
#define prof_active_get JEMALLOC_N(prof_active_get)
#define prof_active_set JEMALLOC_N(prof_active_set)
#define prof_thread_name_get JEMALLOC_N(prof_thread_name_get)
#define prof_thread_name_set JEMALLOC_N(prof_thread_name_set)
#define prof_thread_active_get JEMALLOC_N(prof_thread_active_get)
#define prof_thread_active_set JEMALLOC_N(prof_thread_active_set)
#define prof_thread_active_init_get JEMALLOC_N(prof_thread_active_init_get)
#define prof_thread_active_init_set JEMALLOC_N(prof_thread_active_init_set)
#define prof_gdump_get JEMALLOC_N(prof_gdump_get)
#define prof_gdump_set JEMALLOC_N(prof_gdump_set)
#define prof_boot0 JEMALLOC_N(prof_boot0)
#define prof_boot1 JEMALLOC_N(prof_boot1)
#define prof_boot2 JEMALLOC_N(prof_boot2)
#define prof_prefork0 JEMALLOC_N(prof_prefork0)
#define prof_prefork1 JEMALLOC_N(prof_prefork1)
#define prof_postfork_parent JEMALLOC_N(prof_postfork_parent)
#define prof_postfork_child JEMALLOC_N(prof_postfork_child)
#define prof_sample_new_event_wait JEMALLOC_N(prof_sample_new_event_wait)
#define prof_sample_postponed_event_wait JEMALLOC_N(prof_sample_postponed_event_wait)
#define prof_sample_event_handler JEMALLOC_N(prof_sample_event_handler)
#define prof_bt_hash JEMALLOC_N(prof_bt_hash)
#define prof_bt_keycomp JEMALLOC_N(prof_bt_keycomp)
#define prof_data_init JEMALLOC_N(prof_data_init)
#define prof_bt_count JEMALLOC_N(prof_bt_count)
#define prof_cnt_all JEMALLOC_N(prof_cnt_all)
#define prof_logging_state JEMALLOC_N(prof_logging_state)
#define opt_prof_log JEMALLOC_N(opt_prof_log)
#define opt_prof_recent_alloc_max JEMALLOC_N(opt_prof_recent_alloc_max)
#define edata_prof_recent_alloc_init JEMALLOC_N(edata_prof_recent_alloc_init)
#define edata_prof_recent_alloc_get_no_lock_test JEMALLOC_N(edata_prof_recent_alloc_get_no_lock_test)
#define opt_prof_stats JEMALLOC_N(opt_prof_stats)
#define prof_stats_inc JEMALLOC_N(prof_stats_inc)
#define prof_stats_dec JEMALLOC_N(prof_stats_dec)
#define prof_stats_get_live JEMALLOC_N(prof_stats_get_live)
#define prof_stats_get_accum JEMALLOC_N(prof_stats_get_accum)
#define prof_sys_thread_name_read JEMALLOC_N(prof_sys_thread_name_read)
#define prof_dump_open_file JEMALLOC_N(prof_dump_open_file)
#define prof_dump_write_file JEMALLOC_N(prof_dump_write_file)
#define prof_dump_open_maps JEMALLOC_N(prof_dump_open_maps)
#define prof_do_mock JEMALLOC_N(prof_do_mock)
#define prof_backtrace_hook JEMALLOC_N(prof_backtrace_hook)
#define bt_init JEMALLOC_N(bt_init)
#define prof_backtrace JEMALLOC_N(prof_backtrace)
#define prof_unwind_init JEMALLOC_N(prof_unwind_init)
#define prof_sys_thread_name_fetch JEMALLOC_N(prof_sys_thread_name_fetch)
#define prof_getpid JEMALLOC_N(prof_getpid)
#define prof_get_default_filename JEMALLOC_N(prof_get_default_filename)
#define prof_prefix_set JEMALLOC_N(prof_prefix_set)
#define prof_fdump_impl JEMALLOC_N(prof_fdump_impl)
#define prof_idump_impl JEMALLOC_N(prof_idump_impl)
#define prof_mdump_impl JEMALLOC_N(prof_mdump_impl)
#define prof_gdump_impl JEMALLOC_N(prof_gdump_impl)
#define psset_init JEMALLOC_N(psset_init)
#define psset_stats_accum JEMALLOC_N(psset_stats_accum)
#define psset_update_begin JEMALLOC_N(psset_update_begin)
#define psset_update_end JEMALLOC_N(psset_update_end)
#define psset_pick_alloc JEMALLOC_N(psset_pick_alloc)
#define psset_pick_purge JEMALLOC_N(psset_pick_purge)
#define psset_pick_hugify JEMALLOC_N(psset_pick_hugify)
#define psset_insert JEMALLOC_N(psset_insert)
#define psset_remove JEMALLOC_N(psset_remove)
#define rtree_ctx_data_init JEMALLOC_N(rtree_ctx_data_init)
#define rtree_new JEMALLOC_N(rtree_new)
#define rtree_leaf_elm_lookup_hard JEMALLOC_N(rtree_leaf_elm_lookup_hard)
#define safety_check_fail JEMALLOC_N(safety_check_fail)
#define sc_data_init JEMALLOC_N(sc_data_init)
#define sc_data_update_slab_size JEMALLOC_N(sc_data_update_slab_size)
#define sc_boot JEMALLOC_N(sc_boot)
#define sec_init JEMALLOC_N(sec_init)
#define sec_flush JEMALLOC_N(sec_flush)
#define sec_disable JEMALLOC_N(sec_disable)
#define sec_stats_merge JEMALLOC_N(sec_stats_merge)
#define sec_mutex_stats_read JEMALLOC_N(sec_mutex_stats_read)
#define sec_prefork2 JEMALLOC_N(sec_prefork2)
#define sec_postfork_parent JEMALLOC_N(sec_postfork_parent)
#define sec_postfork_child JEMALLOC_N(sec_postfork_child)
#define global_mutex_names JEMALLOC_N(global_mutex_names)
#define arena_mutex_names JEMALLOC_N(arena_mutex_names)
#define opt_stats_print JEMALLOC_N(opt_stats_print)
#define opt_stats_print_opts JEMALLOC_N(opt_stats_print_opts)
#define opt_stats_interval JEMALLOC_N(opt_stats_interval)
#define opt_stats_interval_opts JEMALLOC_N(opt_stats_interval_opts)
#define stats_interval_new_event_wait JEMALLOC_N(stats_interval_new_event_wait)
#define stats_interval_postponed_event_wait JEMALLOC_N(stats_interval_postponed_event_wait)
#define stats_interval_event_handler JEMALLOC_N(stats_interval_event_handler)
#define stats_print JEMALLOC_N(stats_print)
#define stats_boot JEMALLOC_N(stats_boot)
#define stats_prefork JEMALLOC_N(stats_prefork)
#define stats_postfork_parent JEMALLOC_N(stats_postfork_parent)
#define stats_postfork_child JEMALLOC_N(stats_postfork_child)
#define sz_pind2sz_tab JEMALLOC_N(sz_pind2sz_tab)
#define sz_index2size_tab JEMALLOC_N(sz_index2size_tab)
#define sz_size2index_tab JEMALLOC_N(sz_size2index_tab)
#define sz_psz_quantize_floor JEMALLOC_N(sz_psz_quantize_floor)
#define sz_psz_quantize_ceil JEMALLOC_N(sz_psz_quantize_ceil)
#define opt_tcache JEMALLOC_N(opt_tcache)
#define opt_tcache_max JEMALLOC_N(opt_tcache_max)
#define opt_lg_tcache_nslots_mul JEMALLOC_N(opt_lg_tcache_nslots_mul)
#define opt_tcache_nslots_small_min JEMALLOC_N(opt_tcache_nslots_small_min)
#define opt_tcache_nslots_small_max JEMALLOC_N(opt_tcache_nslots_small_max)
#define opt_tcache_nslots_large JEMALLOC_N(opt_tcache_nslots_large)
#define opt_tcache_gc_incr_bytes JEMALLOC_N(opt_tcache_gc_incr_bytes)
#define opt_tcache_gc_delay_bytes JEMALLOC_N(opt_tcache_gc_delay_bytes)
#define opt_lg_tcache_flush_small_div JEMALLOC_N(opt_lg_tcache_flush_small_div)
#define opt_lg_tcache_flush_large_div JEMALLOC_N(opt_lg_tcache_flush_large_div)
#define tcache_salloc JEMALLOC_N(tcache_salloc)
#define tcache_bin_flush_small JEMALLOC_N(tcache_bin_flush_small)
#define tcache_create_explicit JEMALLOC_N(tcache_create_explicit)
#define tcache_cleanup JEMALLOC_N(tcache_cleanup)
#define tcache_stats_merge JEMALLOC_N(tcache_stats_merge)
#define tcaches_create JEMALLOC_N(tcaches_create)
#define tcaches_flush JEMALLOC_N(tcaches_flush)
#define tcaches_destroy JEMALLOC_N(tcaches_destroy)
#define tcache_boot JEMALLOC_N(tcache_boot)
#define tcache_prefork JEMALLOC_N(tcache_prefork)
#define tcache_postfork_parent JEMALLOC_N(tcache_postfork_parent)
#define tcache_postfork_child JEMALLOC_N(tcache_postfork_child)
#define tcache_flush JEMALLOC_N(tcache_flush)
#define tsd_tcache_data_init JEMALLOC_N(tsd_tcache_data_init)
#define tsd_tcache_enabled_data_init JEMALLOC_N(tsd_tcache_enabled_data_init)
#define tcache_gc_new_event_wait JEMALLOC_N(tcache_gc_new_event_wait)
#define tcache_gc_postponed_event_wait JEMALLOC_N(tcache_gc_postponed_event_wait)
#define tcache_gc_event_handler JEMALLOC_N(tcache_gc_event_handler)
#define tcache_gc_dalloc_new_event_wait JEMALLOC_N(tcache_gc_dalloc_new_event_wait)
#define tcache_gc_dalloc_postponed_event_wait JEMALLOC_N(tcache_gc_dalloc_postponed_event_wait)
#define tcache_gc_dalloc_event_handler JEMALLOC_N(tcache_gc_dalloc_event_handler)
#define test_hooks_arena_new_hook JEMALLOC_N(test_hooks_arena_new_hook)
#define test_hooks_libc_hook JEMALLOC_N(test_hooks_libc_hook)
#define te_assert_invariants_debug JEMALLOC_N(te_assert_invariants_debug)
#define te_event_trigger JEMALLOC_N(te_event_trigger)
#define te_recompute_fast_threshold JEMALLOC_N(te_recompute_fast_threshold)
#define ticker_geom_table JEMALLOC_N(ticker_geom_table)
#define tls_callback JEMALLOC_N(tls_callback)
#define malloc_tsd_malloc JEMALLOC_N(malloc_tsd_malloc)
#define malloc_tsd_dalloc JEMALLOC_N(malloc_tsd_dalloc)
#define malloc_tsd_cleanup_register JEMALLOC_N(malloc_tsd_cleanup_register)
#define malloc_tsd_boot0 JEMALLOC_N(malloc_tsd_boot0)
#define malloc_tsd_boot1 JEMALLOC_N(malloc_tsd_boot1)
#define tsd_cleanup JEMALLOC_N(tsd_cleanup)
#define tsd_fetch_slow JEMALLOC_N(tsd_fetch_slow)
#define tsd_state_set JEMALLOC_N(tsd_state_set)
#define tsd_slow_update JEMALLOC_N(tsd_slow_update)
#define tsd_prefork JEMALLOC_N(tsd_prefork)
#define tsd_postfork_parent JEMALLOC_N(tsd_postfork_parent)
#define tsd_postfork_child JEMALLOC_N(tsd_postfork_child)
#define tsd_global_slow_inc JEMALLOC_N(tsd_global_slow_inc)
#define tsd_global_slow_dec JEMALLOC_N(tsd_global_slow_dec)
#define tsd_global_slow JEMALLOC_N(tsd_global_slow)
#define witness_init JEMALLOC_N(witness_init)
#define witnesses_cleanup JEMALLOC_N(witnesses_cleanup)
#define witness_prefork JEMALLOC_N(witness_prefork)
#define witness_postfork_parent JEMALLOC_N(witness_postfork_parent)
#define witness_postfork_child JEMALLOC_N(witness_postfork_child)
