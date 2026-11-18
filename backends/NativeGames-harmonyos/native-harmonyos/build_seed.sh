#!/bin/sh
# The reference build of the seed. prepare_v143.py's seed step runs build_seed.py instead, which makes this script's
# seed.vkpack byte for byte in a few minutes (2026-11-16, i9-13900HX: 131 s from scratch; this script 55 min, its ~190,000
# intermediate and loose files all written through Docker Desktop's bind mount). Kept to check build_seed.py against
# after the reference's scripts change: same inputs, then compare the two seed.vkpack.
# The Vulkan shader seed for the HarmonyOS port, as android/build_android.py step_shaders / place_shaders / pack_shaders
# make it for Android: every game shader for its default bind state and every bind state of the shared list, translated
# by the reference's totk_xlate (prebuilt/linux-x64) with SPIRV-Cross (the commit build_app.py pins), kept as SPIR-V
# with the Vulkan reflection (android/vk_reflect.py), packed into seed.vkpack (scripts/pack_shader_seed.py).
# Runs in the python:3.14 container (prepare_shader_seed.py needs compression.zstd), below normal priority.
# usage (container): sh build_seed.sh <romfs folder (romfs.bin + index.json)> <rewritten modules> <out folder>
# XLATE=<translator> (default the reference's prebuilt) and SB_ALIGN=<n>: storage buffers addressed relative to
# (base & -n) (native-harmonyos/xlate/build_xlate.sh); the seed folder then gets sb-align.txt = n for the frontend.
# VERSION=v143 (game 1.4.3, the default): the folder <out>/shaders/<version>.
set -eu
VERSION=${VERSION:-v143}
XLATE=${XLATE:-/android/games/totk/prebuilt/linux-x64/totk_xlate}
if [ -n "${SB_ALIGN:-}" ]; then export TOTK_XLATE_SB_ALIGN=$SB_ALIGN; fi
ROMFS=$1; MODULES=$2; OUT=$3
T=/android/games/totk
SC=$OUT/spirv-cross
mkdir -p "$OUT"
if [ ! -x "$SC/spirv-cross" ]; then
  rm -rf "$SC.src"; git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$SC.src"
  git -C "$SC.src" checkout -q aa217aeb6c9f0ace7a0ab233b28807edf45eb165
  mkdir -p "$SC/obj"
  for f in main spirv_cfg spirv_cpp spirv_cross spirv_cross_parsed_ir spirv_cross_util spirv_glsl spirv_hlsl spirv_msl spirv_parser spirv_reflect; do
    echo "$f"; done | xargs -P "$(nproc)" -I{} nice -n 10 g++ -O3 -DNDEBUG -std=c++11 -Wno-deprecated-declarations -c "$SC.src/{}.cpp" -o "$SC/obj/{}.o"
  g++ "$SC"/obj/*.o -o "$SC/spirv-cross"
fi
cd "$T"
nice -n 10 python3 scripts/prepare_shader_seed.py --romfs "$ROMFS" --output "$OUT/seed" --translator "$XLATE" \
  --spirv-cross "$SC/spirv-cross" --version "$VERSION" --modules "$MODULES" --keep-spirv
# Reflection and packing (place_shaders + pack_shaders): <out>/shaders/<version>/{*.spv,*.vk,*.vv,seed.vkpack}
python3 - "$OUT" "$VERSION" <<'EOF'
import os, shutil, sys
from pathlib import Path
sys.path.insert(0, '/android/games/totk/android'); sys.path.insert(0, '/android/games/totk/scripts')
import vk_reflect
from pack_shader_seed import pack_seed
out = Path(sys.argv[1]); seed = out / 'seed'; dst = out / 'shaders' / sys.argv[2]
if dst.exists(): shutil.rmtree(dst)
dst.mkdir(parents=True)
n, bad, unknown = vk_reflect.convert(seed, dst)
for f in seed.glob('*.spv'): shutil.copy2(f, dst / f.name)
marker = seed / 'seed-id.txt'
if marker.is_file(): shutil.copy2(marker, dst / marker.name)
print('variants', n, 'incomplete', bad, 'unclassified', dict(sorted(unknown.items(), key=lambda kv: -kv[1])[:5]))
print('packed', pack_seed(dst))
EOF
if [ -n "${SB_ALIGN:-}" ]; then echo "$SB_ALIGN" > "$OUT/shaders/$VERSION/sb-align.txt"; fi
