#!/bin/sh

which autoconf
returncode=$?
if [ $returncode -ne 0 ]; then
    echo "Error $returncode in autoconf"
    exit 1
fi

echo "./configure --enable-autogen $@"
./configure --enable-autogen $@
returncode=$?
if [ $returncode -ne 0 ]; then
    echo "Error $returncode in ./configure"
    exit $returncode 
fi
