#!/bin/sh
# dav1d (third_party/dav1d: 1.5.4, the arm64 subset seedds/nxnative vendors, BSD 2-Clause) for HarmonyOS, in the
# nativegames-tools:1 container with the SDK sysroot at /ohos/sysroot (as build_vpx_ohos.sh): the AV1 decoder of the
# Switch 2's nn::codec movies (overlay runtime ns2_codec.c / ns2_av1.c; Ocarina of Time's movies are AV1). The files are
# those nxnative's dm/host/Makefile compiles: every src/*.c but the templates, src/arm/cpu.c, the per-bit-depth template
# wrappers in config/tmpl/, every src/arm/64/*.S but the included ones; config-ohos/config.h is the Apple config.h with
# ELF symbols (PREFIX 0) and getauxval CPU detection.
# usage (container): sh build_dav1d_ohos.sh <work folder>     -> <work>/ohos/{include,lib/libdav1d.a}
set -eu
WORK=$1
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/third_party/dav1d
OBJ=$WORK/obj
PREFIX=$WORK/ohos
JOBS=$(python3 "$HERE/loadsched.py" --jobs)
rm -rf "$OBJ" "$PREFIX"; mkdir -p "$OBJ" "$PREFIX/lib" "$PREFIX/include"
CC="clang --target=aarch64-linux-ohos --sysroot=/ohos/sysroot"
CFLAGS="-O2 -fPIC -fvisibility=hidden -fomit-frame-pointer -std=gnu11 -D_GNU_SOURCE -I$SRC/config-ohos -I$SRC -I$SRC/include -ffile-prefix-map=$SRC/=dav1d/"
list=$OBJ/files.txt
: > "$list"
for f in "$SRC"/src/*.c; do case "$f" in *_tmpl.c) ;; *) echo "$f" >> "$list";; esac; done
echo "$SRC/src/arm/cpu.c" >> "$list"
for f in "$SRC"/config/tmpl/*.c; do echo "$f" >> "$list"; done
for f in "$SRC"/src/arm/64/*.S; do case "$f" in *_tmpl.S|*/util.S) ;; *) echo "$f" >> "$list";; esac; done
# config/tmpl wrappers include "src/<name>_tmpl.c" relative to the source root (-I$SRC); config-ohos comes first so its
# config.h is the one found
n=0
while read -r f; do
  n=$((n + 1))
  o=$OBJ/$(echo "${f#$SRC/}" | tr '/' '_').o
  echo "$CC $CFLAGS -c $f -o $o"
done < "$list" > "$OBJ/cmds.sh"
nice -n 10 xargs -P "$JOBS" -I{} sh -c '{}' < "$OBJ/cmds.sh" 2> "$OBJ/errors.txt" || { tail -30 "$OBJ/errors.txt"; exit 1; }
[ "$(ls "$OBJ"/*.o | wc -l)" -eq "$n" ] || { echo "only $(ls "$OBJ"/*.o | wc -l) of $n objects"; tail -30 "$OBJ/errors.txt"; exit 1; }
llvm-ar rcs "$PREFIX/lib/libdav1d.a" "$OBJ"/*.o
cp -r "$SRC/include/dav1d" "$PREFIX/include/"
cp "$SRC/config-ohos/vcs_version.h" "$PREFIX/include/dav1d/" 2>/dev/null || true
llvm-readelf -h "$PREFIX/lib/libdav1d.a" | grep -m1 Machine
echo "dav1d for HarmonyOS: $PREFIX/lib/libdav1d.a ($n objects, $JOBS jobs)"
