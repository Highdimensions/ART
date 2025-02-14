#!/bin/bash
set -e

: ${AOSP=$(realpath $(realpath $(dirname "$0"))/../..)}

TARGET_NAME=linux_glibc_x86
CC="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang"
CXX="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang++"

CFLAGS=\
"--gcc-toolchain=$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8 "\
"--sysroot $AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/sysroot "\
"-fstack-protector -fstack-protector-strong "\
"-target i386-linux-gnu "\
"-B$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/lib/gcc/x86_64-linux/4.8.3/32 "\
"-L$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/lib/gcc/x86_64-linux/4.8.3/32 "\
"-L$AOSP/prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.17-4.8/x86_64-linux/lib32 "

CXXFLAGS=\
"$CFLAGS "\
"-nostdinc++ "\
"-I$AOSP/external/libcxx/include "\
"-I$AOSP/external/libcxxabi/include "\
"-nostdlib++ "\
"-L$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/lib/i386-unknown-linux-gnu "\
"-Wl,-Bstatic "\
"-lc++ "\
"-lc++abi "\
"-Wl,-Bdynamic "

cmake \
  -B build-$TARGET_NAME \
  -GNinja \
  -DCMAKE_INSTALL_PREFIX="$AOSP/art/llvm/install-$TARGET_NAME" \
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
  -DLLVM_BUILD_32_BITS=ON \
  -Wno-dev \
  llvm

cd build-$TARGET_NAME
ninja lib/all $@
ninja install-llvm-headers $@
cd ..
