#!/bin/sh
# The reference shader translator (games/totk/tools/totk_xlate, vendored Ryujinx 1.3.2) with unaligned storage buffers
# for the HarmonyOS GPU: Maleoon's minStorageBufferOffsetAlignment is 256 and the game binds storage buffers at
# 16-byte-aligned guest addresses (~420 bindings a second in the cave, a fifth of them written). With
# TOTK_XLATE_SB_ALIGN=256 in the translator's environment the accessor answers QueryHasUnalignedStorageBuffer() = true
# and QueryHostStorageBufferOffsetAlignment() = 256: Ryujinx's GlobalToStorage then addresses every storage access
# relative to (guest base address & -256), and the backend binds each storage buffer at that aligned address
# (overlay/android/gfx/vk_draw.c ubo_desc). Unset, the translator behaves exactly as the reference's.
# Runs in mcr.microsoft.com/dotnet/sdk:10.0-noble-aot with the reference repository at /android (read-only) and the
# output folder at /out:  sh build_xlate.sh  ->  /out/totk_xlate (+ .sha256)
set -eu
if ! command -v python3 >/dev/null || ! command -v patch >/dev/null; then
  # NX2H_UBUNTU_MIRROR=https://<mirror>/ubuntu (NX2Harmony passes the fastest, nx2h/mirrors.py) takes the place of
  # http://archive.ubuntu.com/ubuntu, and of http://security.ubuntu.com/ubuntu unless NX2H_UBUNTU_SECURITY names
  # another; unset, the image's own sources
  if [ -n "${NX2H_UBUNTU_MIRROR:-}" ]; then
    for f in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do
      if [ -f "$f" ]; then
        sed -i "s|http://archive.ubuntu.com/ubuntu|$NX2H_UBUNTU_MIRROR|g; s|http://security.ubuntu.com/ubuntu|${NX2H_UBUNTU_SECURITY:-$NX2H_UBUNTU_MIRROR}|g" "$f"
      fi
    done
  fi
  # retries, no proxy caching, and a few rounds: an Ubuntu mirror being updated answers "Hash Sum mismatch" for a while
  printf '%s\n' 'Acquire::Retries "3";' 'Acquire::http::Pipeline-Depth "0";' 'Acquire::http::No-Cache "true";' \
    'Acquire::BrokenProxy "true";' > /etc/apt/apt.conf.d/90-reliable
  n=0
  until apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3 patch >/dev/null; do
    n=$((n + 1))
    if [ "$n" -ge 4 ]; then echo "apt-get failed 4 times (network or the Ubuntu mirror): try again later" >&2; exit 1; fi
    rm -rf /var/lib/apt/lists/*; sleep $((n * 20))
  done
fi
W=/tmp/xlate-repo
rm -rf "$W"; mkdir -p "$W/games/totk/tools" "$W/vendor"
cp -R /android/games/totk/tools/totk_xlate "$W/games/totk/tools/totk_xlate"
cp -R /android/vendor/ryujinx "$W/vendor/ryujinx"
# A Windows checkout (core.autocrlf) has CRLF everywhere; the patches were made against LF sources: all text to LF.
find "$W/vendor/ryujinx" "$W/games/totk/tools/totk_xlate" -type f \( -name '*.cs' -o -name '*.csproj' -o -name '*.props' \
  -o -name '*.targets' -o -name '*.patch' -o -name '*.sh' -o -name '*.json' -o -name '*.py' \) -print0 | xargs -0 sed -i 's/\r$//'
P="$W/games/totk/tools/totk_xlate/Program.cs"
python3 - "$P" <<'EOF'
import sys
p = sys.argv[1]; s = open(p, encoding='utf-8').read()
anchor = "        public int QueryTextureArrayLengthFromPool() => 32;\n"
assert s.count(anchor) == 1 and 'QueryHasUnalignedStorageBuffer' not in s
add = anchor + """        // HarmonyOS port: TOTK_XLATE_SB_ALIGN=<n> (n > 16) addresses storage buffers relative to (guest base & -n), for hosts
        // whose storage buffer offset alignment exceeds the guest's 16 (Maleoon 256); the backend binds at that address.
        static readonly int SbAlign = int.TryParse(Environment.GetEnvironmentVariable("TOTK_XLATE_SB_ALIGN"), out int sba) && sba > 16 ? sba : 0;
        public bool QueryHasUnalignedStorageBuffer() => SbAlign != 0;
        public int QueryHostStorageBufferOffsetAlignment() => SbAlign != 0 ? SbAlign : 16;
"""
open(p, 'w', encoding='utf-8').write(s.replace(anchor, add))
print('patched', p)
EOF
# NX2H_NUGET_SOURCE=<v3 index.json> (NX2Harmony, when a NuGet mirror answered faster than nuget.org): the only package
# source of the restore. publish_native.sh builds from a copy under /tmp, so this user-level file decides; unset, nuget.org
if [ -n "${NX2H_NUGET_SOURCE:-}" ]; then
  mkdir -p "$HOME/.nuget/NuGet"
  printf '<?xml version="1.0" encoding="utf-8"?>\n<configuration><packageSources><clear /><add key="nx2h" value="%s" /></packageSources></configuration>\n' \
    "$NX2H_NUGET_SOURCE" > "$HOME/.nuget/NuGet/NuGet.Config"
fi
sh "$W/games/totk/tools/totk_xlate/publish_native.sh" dotnet --out /out
