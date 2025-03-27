#!/bin/bash

# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

: ${AOSP=$(realpath $(realpath $(dirname "$0"))/../../..)}

TARGET_NAME=linux_glibc_x86_64
CC="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang"
CXX="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang++"

CFLAGS=\
"--gcc-toolchain=$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8 "\
"--sysroot $AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot "\
"-fstack-protector -fstack-protector-strong "\
"-target x86_64-linux-gnu "\
"-B$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/lib/gcc/x86_64-linux/4.8.3 "\
"-L$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/lib/gcc/x86_64-linux/4.8.3 "\
"-L$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/x86_64-linux/lib64 "

CXXFLAGS=\
"$CFLAGS "\
"-nostdinc++ "\
"-I$AOSP/external/libcxx/include "\
"-I$AOSP/external/libcxxabi/include "\
"-nostdlib++ "\
"-L$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/lib/x86_64-unknown-linux-gnu "\
"-Wl,-Bstatic "\
"-lc++ "\
"-lc++abi "\
"-Wl,-Bdynamic "

cmake \
  -B build-$TARGET_NAME \
  -GNinja \
  -DCMAKE_INSTALL_PREFIX="$WORKING_DIR/install-$TARGET_NAME" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS -Wno-unused-command-line-argument" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS -Wno-unused-command-line-argument" \
  -DLLVM_ENABLE_PROJECTS="" \
  -DLLVM_ENABLE_RUNTIMES="" \
  -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -Wno-dev \
  llvm

cd build-$TARGET_NAME
ninja lib/all $@
ninja install-llvm-headers $@
cd ..
