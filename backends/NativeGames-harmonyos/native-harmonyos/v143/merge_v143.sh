#!/bin/sh
# 1.4.3 for the HarmonyOS port, step 2 (data), as build_app.py step_update_data does it: the pinned hactool with the
# reference's own source patches (build_app.git_fetch / patch_hactool, imported, not copied), built with gcc here;
# scripts/extract_update_romfs.py merges the base game's RomFS with the update's BKTR patch and verifies every IVFC level;
# the result must equal expected-v1.4.3.json's RomFS hash and size.
# Container: python:3.14 with /android (reference, read-only), /keys, /base (the folder with the base game), /work (as step
# 1: extracted/containers/<update>.nsp exists), /venv, and optionally /xci (a Docker volume): hactool reads the base in
# 4 KiB pieces, about 26 MB/s through a Windows bind mount and several passes, so the base is first copied into the volume
# in 16 MiB blocks (~280 MB/s) and read from there; the copy is removed after a verified merge.
# The base game is one file in /base: a card image .xci (1.0.0, or the 1G+1U card) or the eShop package .nsp, or either
# compressed (.xcz / .nsz). A compressed one is decompressed first with NSZ's hash-verified NCA conversion (the update's
# step 1 does the same), into the volume when there is one; the reference's merge reads an .xci or .nsp alike.
#   sh merge_v143.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=/work
mkdir -p "$W/src" "$W/logs"
/venv/bin/python - <<'EOF'
import sys
from pathlib import Path
sys.path.insert(0, '/android')
import build_app as b
src = Path('/work/src/hactool')
b.git_fetch(*b.HACTOOL, src, Path('/work/logs/fetch-hactool.log'))
b.patch_hactool(src, Path('/work/logs'))
print('hactool source', b.HACTOOL[1][:12], 'patched')
EOF
cd "$W/src/hactool"
[ -f config.mk ] || cp config.mk.template config.mk
make -j8 CC=gcc > "$W/logs/hactool-build.log" 2>&1 || { tail -20 "$W/logs/hactool-build.log"; exit 1; }
BASE=
for f in /base/*.[xX][cC][iI] /base/*.[nN][sS][pP] /base/*.[xX][cC][zZ] /base/*.[nN][sS][zZ]; do
  if [ -f "$f" ]; then BASE=$f; break; fi
done
if [ -z "$BASE" ]; then echo "no base game (.xci / .nsp / .xcz / .nsz) in /base" >&2; exit 1; fi
# where a decompressed base goes: the volume when there is one, else a folder of its own (extracted/containers holds the
# update's .nsp, which the merge below finds by its suffix)
TMP=/work/extracted/base
if [ -d /xci ]; then TMP=/xci; fi
mkdir -p "$TMP"
LOCAL=
case "$BASE" in
  *.[nN][sS][zZ]|*.[xX][cC][zZ])
    case "$BASE" in *.[nN][sS][zZ]) EXT=nsp; SUF=nsz ;; *) EXT=xci; SUF=xcz ;; esac
    STEM=$(basename "${BASE%.*}")
    LOCAL="$TMP/$STEM.$EXT"
    # the mark names the source the file was made from (name, size, time): a decompression that stopped half-way, or one
    # of another file under the same name, is done again
    ID="$(basename "$BASE") $(stat -c '%s %Y' "$BASE")"
    if [ "$(cat "$LOCAL.done" 2>/dev/null || true)" != "$ID" ]; then
      # an earlier run's files go first: nsz skips a title it finds already decompressed in the output folder
      rm -rf "$TMP"/* 2>/dev/null || true
      SRC=$BASE
      case "$BASE" in
        *.nsz|*.xcz) ;;
        *) mkdir -p "$TMP/in"; SRC="$TMP/in/$STEM.$SUF"; ln -s "$BASE" "$SRC" ;;   # nsz takes lower-case suffixes only
      esac
      echo "decompressing the base $(basename "$BASE") (hash-verified NCA conversion)"
      /venv/bin/nsz -D --keys /keys/prod.keys -o "$TMP" "$SRC" > "$W/logs/decompress-base.log" 2>&1 \
        || { tail -30 "$W/logs/decompress-base.log"; exit 1; }
      rm -rf "$TMP/in"
      # nsz -D reports an NCA whose hash does not match as [CORRUPTED] and still exits 0
      if grep -q 'CORRUPTED' "$W/logs/decompress-base.log"; then
        grep 'CORRUPTED' "$W/logs/decompress-base.log" | head -5 >&2
        echo "the base $(basename "$BASE") failed NSZ's NCA hash check: a damaged download or dump" >&2
        rm -f "$LOCAL"; exit 1
      fi
      if [ ! -s "$LOCAL" ]; then echo "nsz wrote no $(basename "$LOCAL")" >&2; ls -la "$TMP" >&2; exit 1; fi
      printf '%s\n' "$ID" > "$LOCAL.done"
    fi
    BASE=$LOCAL ;;
  *)
    if [ -d /xci ]; then
      LOCAL="/xci/$(basename "$BASE")"
      for f in /xci/*; do   # an earlier run's other copies: not kept beside a new 16 GB one
        if [ -e "$f" ] && [ "$f" != "$LOCAL" ]; then rm -rf "$f"; fi
      done
      if [ "$(stat -c %s "$LOCAL" 2>/dev/null || echo 0)" != "$(stat -c %s "$BASE")" ]; then
        echo "copying the base game into the volume"; dd if="$BASE" of="$LOCAL" bs=16M status=none
      fi
      BASE=$LOCAL
    fi ;;
esac
NSP=$(ls "$W"/extracted/containers/*.nsp | head -1)
echo "base $(basename "$BASE"), update $(basename "$NSP")"
if [ ! -s "$W/extracted/v143/romfs/romfs.bin" ]; then
  rm -rf "$W/extracted/v143/romfs"
  cd /android/games/totk
  /venv/bin/python "$HERE/merge_base_xci.py" "$BASE" "$NSP" --keys /keys/prod.keys --hactool "$W/src/hactool/hactool" \
    --output "$W/extracted/v143/romfs" > "$W/logs/update-romfs.log" 2>&1 || { tail -30 "$W/logs/update-romfs.log"; exit 1; }
fi
/venv/bin/python - <<'EOF'
import json
from pathlib import Path
want = json.loads(Path('/android/games/totk/assets/expected-v1.4.3.json').read_text())['romfs']
r = json.loads(Path('/work/extracted/v143/romfs/index.json').read_text())
ok = r.get('romfs_sha256') == want['sha256'] and r.get('romfs_size') == want['size']
print('merged RomFS', r.get('romfs_size'), r.get('romfs_sha256', '')[:16], 'matches 1.4.3' if ok else 'DIFFERS from 1.4.3')
raise SystemExit(0 if ok else 1)
EOF
# the copy or the decompressed base, once the merge is verified (and whatever an earlier stopped run left in the volume)
if [ -n "$LOCAL" ]; then rm -f "$LOCAL" "$LOCAL.done"; fi
rmdir "$W/extracted/base" 2>/dev/null || true
if [ -d /xci ]; then rm -rf /xci/*; fi
