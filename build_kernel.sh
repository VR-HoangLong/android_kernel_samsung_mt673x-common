#!/bin/bash

export CROSS_COMPILE=$(pwd)/../arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
export CC=$(pwd)/../arm-linux-androideabi-4.9/bin/arm-linux-androideabi-gcc
export CLANG_TRIPLE=arm-linux-androideabi-gcc
export ARCH=arm

export KCFLAGS=-w
export CONFIG_SECTION_MISMATCH_WARN_ONLY=y

make -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y a01core_defconfig
make -C $(pwd) O=$(pwd)/out KCFLAGS=-w CONFIG_SECTION_MISMATCH_WARN_ONLY=y -j4

cp out/arch/arm/boot/zImage $(pwd)/arch/arm/boot/zImage
cp out/arch/arm/boot/zImage $(pwd)/arch/arm64/boot/zImage
