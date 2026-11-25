#!/bin/sh

if [ "x${enable_xmalloc}" = "x1" ] ; then
  export MALLOC_CONF="xmalloc:false"
fi
