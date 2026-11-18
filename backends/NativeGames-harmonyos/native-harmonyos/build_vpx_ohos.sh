#!/bin/sh
# libvpx's VP9 decoder for HarmonyOS (the Android port's movie_host.c plays the game's VP9 cutscenes with it), in the
# nativegames-tools:1 container. Same source and options as android/build_android.py build_vpx (v1.15.0, VP9 decoder only,
# runtime CPU detection). libvpx's configure links test programs, which the HarmonyOS sysroot cannot do with the
# container's clang, so it is configured as generic arm64 Linux with the aarch64 cross gcc; the library is then compiled
# with clang for aarch64-linux-ohos against the SDK sysroot (/ohos/sysroot): only objects and an archive, no linking.
# usage (container): sh build_vpx_ohos.sh <work folder>     -> <work>/ohos/{include,lib/libvpx.a}
set -eu
WORK=$1
SRC=$WORK/src/libvpx
BUILD=$WORK/build-ohos
PREFIX=$WORK/ohos
HERE=$(dirname "$0")
JOBS=$(python3 "$HERE/loadsched.py" --jobs)
if [ ! -f "$SRC/configure" ]; then
  git clone --depth 1 -b v1.15.0 https://chromium.googlesource.com/webm/libvpx "$SRC"
fi
test "$(git -C "$SRC" describe --tags)" = v1.15.0
rm -rf "$BUILD" "$PREFIX"; mkdir -p "$BUILD"; cd "$BUILD"
CROSS=aarch64-linux-gnu- "$SRC/configure" --target=arm64-linux-gcc --prefix="$PREFIX" --enable-pic --disable-examples \
  --disable-tools --disable-docs --disable-unit-tests --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder \
  --disable-webm-io --disable-libyuv --enable-runtime-cpu-detect
OHOS_CC="clang --target=aarch64-linux-ohos --sysroot=/ohos/sysroot -ffile-prefix-map=$SRC/=libvpx/ -ffile-prefix-map=$BUILD/=libvpx-build/"
nice -n 10 make -j"$JOBS" CC="$OHOS_CC" CXX="$OHOS_CC" AS="$OHOS_CC" AR=llvm-ar NM=llvm-nm RANLIB=llvm-ranlib STRIP=llvm-strip libvpx.a
make CC="$OHOS_CC" CXX="$OHOS_CC" AS="$OHOS_CC" AR=llvm-ar NM=llvm-nm RANLIB=llvm-ranlib STRIP=llvm-strip install-libs install-srcs 2>/dev/null || \
make CC="$OHOS_CC" CXX="$OHOS_CC" AS="$OHOS_CC" AR=llvm-ar NM=llvm-nm RANLIB=llvm-ranlib STRIP=llvm-strip install
llvm-readelf -h "$PREFIX/lib/libvpx.a" | grep -m1 Machine
echo "libvpx for HarmonyOS: $PREFIX/lib/libvpx.a ($JOBS jobs)"
