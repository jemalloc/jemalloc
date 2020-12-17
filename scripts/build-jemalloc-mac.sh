#!/bin/bash -xe
# Jemalloc source code in PROJECT_HOME.
if [ -z "$PROJECT_HOME" ]
then
      echo "Provide a \$PROJECT_HOME"
      exit 1
else
      echo "Building libjemalloc from $PROJECT_HOME"
fi

cd $PROJECT_HOME

export CFLAGS="-O3 -g3 -funroll-loops -DENABLE_MALLOC_ANALYZER"
export CXXFLAGS="-O3 -g3 -DENABLE_MALLOC_ANALYZER"
export LDFLAGS="-stdlib=libc++"

./autogen.sh --enable-debug --enable-fill --enable-prof --enable-stats --disable-cache-oblivious 
# To rename libjemalloc.2.dylib to libjemalloc.dylib
sed -i'.bak' 's/SOREV = 2.dylib/SOREV = dylib/g' Makefile
# Add install_name for the main binary to pick up.
sed -i'.bak' 's/DSO_LDFLAGS = .*/DSO_LDFLAGS = -shared -Wl,-install_name,@rpath\/Jemalloc.framework\/$(@F) $(LDFLAGS)/g' Makefile
rm -f Makefile.bak
make -j4
