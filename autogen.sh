#!/bin/sh

which autoconf
returncode=$?
if [ $returncode -ne 0 ]; then
    echo "Error $returncode in autoconf"
    exit $returncode 
fi

echo "./configure --enable-autogen $@"
./configure --enable-autogen $@
returncode=$?
if [ $returncode -ne 0 ]; then
    echo "Error $returncode in ./confinure"
    exit $returncode 
fi
