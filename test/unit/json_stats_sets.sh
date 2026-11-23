#!/bin/sh

export MALLOC_CONF="hpa_shard_sets:true,hpa_shard_sets_nshards:2|0|3,background_thread:false"
