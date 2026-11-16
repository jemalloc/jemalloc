#!/bin/sh

if [ "x${enable_fill}" = "x1" ] ; then
    export MALLOC_CONF="junk:false"
fi
if [ "x${enable_xmalloc}" = "x1" ] ; then
    export MALLOC_CONF="${MALLOC_CONF:+${MALLOC_CONF},}xmalloc:false"
fi
