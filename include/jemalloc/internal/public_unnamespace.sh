#!/bin/sh

for symbol in `cat $1` ; do
  echo "#undef je_${symbol}"
done
