#! /bin/env bash

./autogen.sh
mkdir -p build
cd build
CFLAGS="-ggdb -O0" \
    ../configure \
    --with-llvm=/usr/lib/llvm-$LLVM_VERSION/bin/llvm-config \
    --enable-debug
cd ..
