#!/bin/bash
set -e

: ${AOSP=$(realpath $(realpath $(dirname "$0"))/../..)}

TARGET_NAME=android_arm64
CC="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang"
CXX="$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/clang++"

CFLAGS=\
"-fstack-protector-strong "\
"-nostdlibinc "\
"-target aarch64-linux-android31 "\
"-march=armv8-a "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/platform "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/async_safe/include "\
"-I$AOSP/prebuilts/runtime/mainline/runtime/sdk/common_os/include/bionic/libc/system_properties/include "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/include/bionic/libc/include "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/include/bionic/libc/kernel/uapi/asm-arm64 "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/include/bionic/libc/kernel/uapi "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/include/bionic/libc/kernel/android/scsi "\
"-isystem $AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/include/bionic/libc/kernel/android/uapi "\
"-B$AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/lib "\
"-L$AOSP/prebuilts/runtime/mainline/runtime/sdk/android/arm64/lib "

CXXFLAGS=\
"$CFLAGS "\
"-nostdinc++ "\
"-nostdlib "\
"-nostdlib++ "\
"-I$AOSP/external/libcxx/include "\
"-I$AOSP/external/libcxxabi/include "\
"-L$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/android_libc++/platform/aarch64/lib "\
"-lc++ "\
"-lc++abi "

cmake \
  -B build-$TARGET_NAME \
  -GNinja \
  -DCMAKE_INSTALL_PREFIX="$AOSP/art/llvm/install-$TARGET_NAME" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS -Wno-unused-command-line-argument" \
  -DCMAKE_CXX_FLAGS="$CXXFLAGS -Wno-unused-command-line-argument" \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_NATIVE_TOOL_DIR=build-linux_glibc_x86_64/bin \
  -DLLVM_ENABLE_PROJECTS="" \
  -DLLVM_ENABLE_RUNTIMES="" \
  -DLLVM_ENABLE_TERMINFO=OFF \
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
  -DLLVM_HOST_TRIPLE=aarch64-linux-android31 \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DHAVE_CXX_ATOMICS_WITHOUT_LIB=TRUE \
  -DHAVE_CXX_ATOMICS64_WITHOUT_LIB=TRUE \
  -Wno-dev \
  llvm

cd build-$TARGET_NAME
ninja lib/all $@
ninja install-llvm-headers $@
cd ..
