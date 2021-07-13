#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,lg_prof_sample:0,prof_select_usize:32,prof_select_waste:12"
fi
