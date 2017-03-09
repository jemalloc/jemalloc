#!/bin/sh

# Immediately purge to minimize fragmentation.
export MALLOC_CONF="dirty_decay_time:0,muzzy_decay_time:0"
