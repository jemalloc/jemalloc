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

./autogen.sh --target="aarch64-apple-ios" --host="aarch64-apple-ios" --with-lg-page=14 --enable-static=no
SYSROOT=`xcrun --sdk iphoneos --show-sdk-path | head -1`
INCLUDE=$SYSROOT/usr/include

# CFLAGS_SYM = $(shell echo $(CFLAGS) | sed 's/.arch armv7//g')
# CFLAGS_SYM is only used for getting symbol names of jemalloc functions and as such does not require multiple
# architecture. Adding both architecture does not work well with nm.
# -gdwarf-2 and -Wl,-object_path_lto,$TMPLTO are there to help keep debug symbols that can be put into dSYM files.

export CFLAGS_SYM="-O3 -flto -gdwarf-2 -mios-version-min=10.0 -arch arm64 -isysroot $SYSROOT -I$INCLUDE -stdlib=libc++"

TMPLTO=`pwd`/tmplto
mkdir -p $TMPLTO
rm -rf $TMPLTO/*

cd $PROJECT_HOME
./configure --target="aarch64-apple-ios" --host="aarch64-apple-ios" \
  --with-lg-page=14 --enable-static=no --disable-cache-oblivious \
  CC=clang \
  CXX=clang++ \
  CFLAGS="-O3 -flto -gdwarf-2 -mios-version-min=10.0 -funroll-loops -arch armv7 -arch arm64 -isysroot $SYSROOT -I$INCLUDE -stdlib=libc++" \
  CXXFLAGS="-O3 -flto -gdwarf-2 -mios-version-min=10.0 -arch armv7 -arch arm64 -isysroot $SYSROOT -I$INCLUDE -stdlib=libc++" \
  LDFLAGS="-O3 -flto -gdwarf-2 -arch armv7 -arch arm64 -mios-version-min=10.0 -isysroot $SYSROOT -stdlib=libc++ -Wl,-object_path_lto,$TMPLTO"

# To rename libjemalloc.2.dylib to libjemalloc.dylib
sed -i'.bak' 's/SOREV = 2.dylib/SOREV = dylib/g' Makefile
# Add install_name for the main binary to pick up.
sed -i'.bak' 's/DSO_LDFLAGS = .*/DSO_LDFLAGS = -shared -Wl,-install_name,@rpath\/Jemalloc.framework\/$(@F) $(LDFLAGS)/g' Makefile
rm Makefile.bak

make -j4 && make install DESTDIR=`pwd`/install/ios/

# Debug symbols
LIBJEMALLOC=`pwd`/install/ios/usr/local/lib/libjemalloc.dylib
dsymutil $LIBJEMALLOC
zip -r $LIBJEMALLOC.dSYM.zip $LIBJEMALLOC.dSYM
rm -rf $LIBJEMALLOC.dSYM
