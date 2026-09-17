#!/bin/bash

set -ev

if [[ "$TRAVIS_OS_NAME" != "linux" ]]; then
    echo "Incorrect \$TRAVIS_OS_NAME: expected linux, got $TRAVIS_OS_NAME"
    exit 1
fi

sudo apt-get update
sudo apt-get install -y libunwind-dev
if [[ "$CROSS_COMPILE_32BIT" == "yes" ]]; then
    sudo apt-get -y install gcc-multilib g++-multilib
fi
