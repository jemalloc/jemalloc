#!/bin/sh

export MALLOC_CONF="hpa_shard_sets:true,hpa_shard_sets_nshards:2|0|3,hpa_dirty_mult:-1,background_thread:false"
