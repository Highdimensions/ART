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

LLVM_VERSION=20.1.0

# The script is in the directory $AOSP/art/compiler/llvm, so $AOSP is 3 directories down.
: ${AOSP=$(realpath $(dirname "$0"))/../../..}
export AOSP=$(realpath "$AOSP")
# Use the newest revision of the prebuilt toolchains.
export TOOLCHAIN_DIR=$(find "$AOSP/prebuilts/clang/host/linux-x86" -name 'clang-r*' -type d | sort -V | tail -n1)

export WORKING_DIR=$(realpath $(dirname "$0"))
NINJA_ARGS=$@

get_source() {
  if [ ! -d llvm ]; then
    mkdir llvm
    curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-$LLVM_VERSION.src.tar.xz" | tar -xJ -C llvm --strip-components=1
  fi

  if [ ! -d cmake ]; then
    mkdir cmake
    curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/cmake-$LLVM_VERSION.src.tar.xz" | tar -xJ -C cmake --strip-components=1
  fi

  for p in $(find llvm-patches -type f -exec realpath {} \; | sort); do
    if patch -p1 < "$p" > /dev/null 2>&1; then
      echo "Applied patch $p"
    else
      echo "Failed to apply patch $p" > /dev/null
      exit 1
    fi
  done
}

build() {
  TARGET=$1
  "$WORKING_DIR"/build_$TARGET.sh $NINJA_ARGS

  mkdir -p $TARGET
  cd $TARGET

  cp -r "$WORKING_DIR"/install-$TARGET/include .

  mkdir -p lib
  cd lib

  rm -f libLLVM.a
  # Include every archive file, except for JIT-specific ones.
  LIBFILES=$(find "$WORKING_DIR/build-$TARGET/lib" -name 'libLLVM*.a' | sed 's/.*libLLVM\(Orc\w\+\|\w*JIT\w*\)\.a//g')
  "$TOOLCHAIN_DIR/bin/llvm-ar" cqsL libLLVM.a $LIBFILES

  cd ../..
}

cd $WORKING_DIR

get_source

for TARGET in linux_glibc_x86 linux_glibc_x86_64 android_arm64 android_arm; do
  build $TARGET
done