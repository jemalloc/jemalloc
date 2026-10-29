#!/bin/sh

export MALLOC_CONF="hpa_shard_sets:true,hpa_shard_sets_configs:1-2097152:1,hpa_shard_sets_configs:1-16384:4|16385-65536:8;roundrobin|65537-2097152:2;arena"
