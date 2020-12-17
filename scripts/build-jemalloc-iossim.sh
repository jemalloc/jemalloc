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

SYSROOT=`xcrun --sdk iphonesimulator --show-sdk-path | head -1`
INCLUDE=$SYSROOT/usr/include

export CFLAGS="-O3 -g3 -funroll-loops -arch x86_64 -fobjc-abi-version=2 -mios-simulator-version-min=10.0 -DLOCAL_BUILD -isysroot $SYSROOT -I$INCLUDE"
export CXXFLAGS="-O3 -g3 -arch x86_64 -fobjc-abi-version=2 -mios-simulator-version-min=10.0 -DLOCAL_BUILD -isysroot $SYSROOT -I$INCLUDE"
export LDFLAGS="-arch x86_64 -fobjc-abi-version=2 -mios-simulator-version-min=10.0 -isysroot $SYSROOT -stdlib=libc++"

./autogen.sh --enable-debug --enable-fill --enable-prof --enable-stats --host=x86_64-apple-ios --disable-cache-oblivious 

# To rename libjemalloc.2.dylib to libjemalloc.dylib
sed -i'.bak' 's/SOREV = 2.dylib/SOREV = dylib/g' Makefile

# Add install_name for the main binary to pick up.
sed -i'.bak' 's/DSO_LDFLAGS = .*/DSO_LDFLAGS = -shared -Wl,-install_name,@rpath\/Jemalloc.framework\/$(@F) $(LDFLAGS)/g' Makefile
rm -f Makefile.bak

make -j4 && make install DESTDIR=`pwd`/install/iossim/
# Debug symbols
LIBJEMALLOC=`pwd`/install/iossim/usr/local/lib/libjemalloc.dylib
dsymutil $LIBJEMALLOC
zip -r $LIBJEMALLOC.dSYM.zip $LIBJEMALLOC.dSYM
rm -rf $LIBJEMALLOC.dSYM
