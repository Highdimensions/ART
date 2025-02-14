#!/bin/bash
set -e

# The script is in the directory $AOSP/art/llvm, so $AOSP is 2 directories down.
: ${AOSP=$(realpath $(dirname "$0"))/../..}
export AOSP=$(realpath "$AOSP")

MY_DIR=$(realpath $(dirname "$0"))
NINJA_ARGS=$@

get_source() {
  if [ ! -d "$MY_DIR/llvm" ]; then
    curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.6/llvm-18.1.6.src.tar.xz" | tar -xJ --transform='s/llvm-18.1.6.src/llvm/'
  fi
  if [ ! -d "$MY_DIR/cmake" ]; then
    curl -L "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.6/cmake-18.1.6.src.tar.xz" | tar -xJ --transform='s/cmake-18.1.6.src/cmake/'
  fi

  for p in $(find "$MY_DIR/llvm-patches" -type f -exec realpath {} \; | sort); do
    if patch -p1 < "$p" > /dev/null 2>&1; then
      echo "Applied patch $p"
    else
      echo "Failed to apply patch $p" > /dev/null
    fi
  done
}

build() {
  TARGET=$1
  "$MY_DIR"/build_$TARGET.sh $NINJA_ARGS

  mkdir -p $TARGET
  cd $TARGET

  cp -r "$MY_DIR"/install-$TARGET/include .

  mkdir -p lib
  cd lib

  rm -f libLLVM.a
  # Include every archive file, except for JIT-specific ones.
  LIBFILES=$(find "$MY_DIR"/build-$TARGET/lib -name libLLVM*.a | sed 's/.*libLLVM\(Orc\w\+\|\w*JIT\w*\)\.a//g')
  "$AOSP/prebuilts/clang/host/linux-x86/clang-r530567/bin/llvm-ar" cqsL libLLVM.a $LIBFILES

  cd ../..
}

get_source

for TARGET in linux_glibc_x86 linux_glibc_x86_64 android_arm64; do
  build $TARGET
done
