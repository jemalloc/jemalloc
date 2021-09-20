#!/bin/sh

prefix=
exec_prefix=
libdir=${exec_prefix}/lib

LD_PRELOAD=${libdir}/libjemalloc.dll
export LD_PRELOAD
exec "$@"
