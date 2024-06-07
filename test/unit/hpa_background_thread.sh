#!/bin/sh

export MALLOC_CONF="hpa_dirty_mult:0.001,hpa_hugification_threshold_ratio:1.0,hpa_min_purge_interval_ms:50,hpa_strict_min_purge_interval:true,hpa_sec_nshards:0"

