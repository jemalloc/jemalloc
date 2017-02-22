#!/bin/sh

# Immediately purge to minimize fragmentation.
export MALLOC_CONF="decay_time:-1"
