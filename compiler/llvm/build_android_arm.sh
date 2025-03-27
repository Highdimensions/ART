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

TARGET_NAME=android_arm
CC="$TOOLCHAIN_DIR/bin/clang"
CXX="$TOOLCHAIN_DIR/bin/clang++"

CFLAGS=\
"-fstack-protector-strong "\
"-nostdlibinc "\
"-nostdlib "\
"-mthumb "\
"-ffp-contract=off "\
"-fno-short-enums "\
"-target armv7a-linux-androideabi31 "\
"-msoft-float "\
"-mfloat-abi=softfp "\
"-mfpu=neon-fp-armv8 "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/platform "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/async_safe/include "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/system_properties/include "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/include/bionic/libc/include "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/include/bionic/libc/kernel/uapi/asm-arm "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/include/bionic/libc/kernel/uapi "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/include/bionic/libc/kernel/android/scsi "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/include/bionic/libc/kernel/android/uapi "\
"-B$AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/lib "\
"-L$AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm/lib "\
"-lc "

CXXFLAGS=\
"$CFLAGS "\
"-nostdinc++ "\
"-nostdlib++ "\
"-I$TOOLCHAIN_DIR/android_libc++/platform/arm/include/c++/v1 "\
"-I$TOOLCHAIN_DIR/include/c++/v1 "\
"-L$TOOLCHAIN_DIR/android_libc++/platform/arm/lib "\
"-lc++ "\
"-lc++abi "

cmake \
  -B build-$TARGET_NAME \
  -GNinja \
  -DCMAKE_INSTALL_PREFIX="$WORKING_DIR/install-$TARGET_NAME" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS -Wno-unused-command-line-argument" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS -Wno-unused-command-line-argument" \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_NATIVE_TOOL_DIR=build-linux_glibc_x86_64/bin \
  -DLLVM_ENABLE_PROJECTS="" \
  -DLLVM_ENABLE_RUNTIMES="" \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_BACKTRACES=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_TOOLS=OFF \
  -DLLVM_INCLUDE_UTILS=OFF \
  -DLLVM_BUILD_TESTS=OFF \
  -DLLVM_BUILD_BENCHMARKS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_USE_HOST_TOOLS=ON \
  -DLLVM_HOST_TRIPLE=armv7a-linux-androideabi31 \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -Wno-dev \
  llvm

cd build-$TARGET_NAME
ninja lib/all $@
ninja install-llvm-headers $@
cd ..