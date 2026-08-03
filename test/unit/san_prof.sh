#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,lg_prof_sample:0,san_guard_large:1,san_guard_small:0"
fi
