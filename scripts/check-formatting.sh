#!/bin/bash

# The files that need to be properly formatted.  We'll grow this incrementally
# until it includes all the jemalloc source files (as we convert things over),
# and then just replace it with
#    find -name '*.c' -o -name '*.h' -o -name '*.cpp
FILES=($(find ./test/unit/ -name '*.c' -o -name '*.h' -o -name '*.cpp'))
STYLE=file

if command -v clang-format &> /dev/null; then
  CLANG_FORMAT="clang-format"
else
  echo "Couldn't find clang-format."
fi

$CLANG_FORMAT --version
for file in ${FILES[@]}; do
  if ! cmp --silent $file <($CLANG_FORMAT $file --style=$STYLE) &> /dev/null; then
    echo "Error: $file is not clang-formatted"
    exit 1
  fi
done
