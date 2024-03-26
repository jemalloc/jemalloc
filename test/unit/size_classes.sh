#!/bin/sh

if [ "x${limit_usize_gap}" = "x1" ] ; then
  export MALLOC_CONF="limit_usize_gap:true"
fi
