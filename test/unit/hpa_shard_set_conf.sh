#!/bin/sh

export MALLOC_CONF="hpa_shard_sets:true,hpa_shard_sets_nshards:1,hpa_shard_sets_nshards:4|4|0|2"
