#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,prof_accum:true,prof_active:false,lg_prof_sample:0,lg_prof_interval:0"
  if [ "x${enable_tcache}" = "x1" ] ; then
    export MALLOC_CONF="${MALLOC_CONF},tcache:false"
  fi
elif [ "x${enable_tcache}" = "x1" ] ; then
  export MALLOC_CONF="tcache:false"
fi


