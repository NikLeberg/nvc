#! /bin/env bash

mkdir -p build
cd build
make -j$(nproc)
make install
cd ..
