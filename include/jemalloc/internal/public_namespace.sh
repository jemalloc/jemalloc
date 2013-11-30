#!/bin/sh

for symbol in `cat $1` ; do
  echo "#define	je_${symbol} JEMALLOC_N(${symbol})"
done
