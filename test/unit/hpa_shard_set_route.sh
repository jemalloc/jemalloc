#!/bin/sh

export MALLOC_CONF="hpa_shard_sets:true,hpa_shard_sets_nshards:2|0|8,hpa_dirty_mult:0,hpa_min_purge_interval_ms:0,dirty_decay_ms:0,muzzy_decay_ms:0,background_thread:false"
