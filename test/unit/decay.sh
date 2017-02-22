#!/bin/sh

export MALLOC_CONF="decay_time:1"
if [ "x${enable_tcache}" = "x1" ] ; then
  export MALLOC_CONF="${MALLOC_CONF},lg_tcache_max:0"
fi
