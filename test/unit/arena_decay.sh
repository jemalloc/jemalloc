#!/bin/sh

export MALLOC_CONF="dirty_decay_ms:1000,muzzy_decay_ms:1000,tcache_max:1024,pac_sec_nshards:0"
